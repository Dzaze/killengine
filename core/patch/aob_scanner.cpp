#include "aob_scanner.h"

#include "logging/logger.h"
#include "memory/memory_map.h"
#include "memory/memory_reader.h"

#include <algorithm>
#include <array>

namespace killcore {

namespace {

bool regionMatchesAobOptions(const MemoryRegion& region, const AobScanOptions& options) {
    if (region.state != MemoryState::Committed || !region.readable || region.guarded) return false;
    if (options.executableOnly && !region.executable) return false;
    if (options.writableOnly && !region.writable) return false;
    if (options.imageOnly && region.type != MemoryType::Image) return false;
    if (options.stopAddress > 0 && region.baseAddress >= options.stopAddress) return false;
    if (options.startAddress > 0 && region.baseAddress + region.size <= options.startAddress) return false;
    return true;
}

int hexNibble(QChar ch) {
    const ushort c = ch.toUpper().unicode();
    if (c >= '0' && c <= '9') return static_cast<int>(c - '0');
    if (c >= 'A' && c <= 'F') return static_cast<int>(c - 'A' + 10);
    return -1;
}

} // namespace

AobPattern parseAobPattern(const QString& patternText) {
    AobPattern pattern;
    const QStringList tokens = patternText.simplified().split(' ', Qt::SkipEmptyParts);
    if (tokens.isEmpty()) {
        pattern.error = "Pattern AOB vide.";
        return pattern;
    }

    for (const QString& rawToken : tokens) {
        QString token = rawToken.trimmed();
        if (token.startsWith("0x", Qt::CaseInsensitive)) {
            token = token.mid(2);
        }

        if (token == "?" || token == "??") {
            pattern.bytes.push_back(std::nullopt);
            continue;
        }

        if (token.size() != 2) {
            pattern.error = QString("Token AOB invalide: '%1'. Utilise par exemple '48 8B ?? 10'.").arg(rawToken);
            pattern.bytes.clear();
            return pattern;
        }

        const int hi = hexNibble(token.at(0));
        const int lo = hexNibble(token.at(1));
        if (hi < 0 || lo < 0) {
            pattern.error = QString("Octet hex invalide: '%1'.").arg(rawToken);
            pattern.bytes.clear();
            return pattern;
        }

        pattern.bytes.push_back(static_cast<uint8_t>((hi << 4) | lo));
    }

    return pattern;
}

QString bytesToAobPattern(const QByteArray& bytes) {
    return QString::fromLatin1(bytes.toHex(' ').toUpper());
}

AobPatternQuality evaluateAobPatternQuality(const AobPattern& pattern) {
    AobPatternQuality quality;
    quality.patternBytes = static_cast<int>(pattern.bytes.size());
    if (!pattern.isValid()) {
        quality.level = "invalid";
        quality.warning = pattern.error.isEmpty() ? "Pattern AOB invalide." : pattern.error;
        return quality;
    }

    std::array<bool, 256> seen{};
    for (const auto& byte : pattern.bytes) {
        if (byte) {
            ++quality.fixedBytes;
            if (!seen[*byte]) {
                seen[*byte] = true;
                ++quality.uniqueFixedBytes;
            }
        } else {
            ++quality.wildcardBytes;
        }
    }

    quality.fixedRatio = quality.patternBytes > 0
        ? static_cast<double>(quality.fixedBytes) / static_cast<double>(quality.patternBytes)
        : 0.0;

    int score = 0;
    score += std::min(40, quality.fixedBytes * 4);
    score += std::min(25, quality.uniqueFixedBytes * 3);
    score += std::min(20, quality.patternBytes);
    score += static_cast<int>(quality.fixedRatio * 15.0);
    if (quality.patternBytes < 6) score -= 25;
    if (quality.fixedBytes < 4) score -= 35;
    if (quality.uniqueFixedBytes < 3) score -= 15;
    if (quality.fixedRatio < 0.45) score -= 20;
    if (quality.wildcardBytes == quality.patternBytes) score = 0;

    quality.score = std::clamp(score, 0, 100);
    if (quality.score >= 75) {
        quality.level = "strong";
        quality.warning = "Signature AOB robuste; verifier quand meme l'unicite avant Trainer.";
    } else if (quality.score >= 50) {
        quality.level = "medium";
        quality.warning = "Signature AOB moyenne; preferer une fenetre plus longue ou plus d'octets fixes.";
    } else {
        quality.level = "weak";
        quality.warning = "Signature AOB faible; risque eleve de multi-match ou de casse apres update.";
    }
    quality.trainerSafe = quality.score >= 75 && quality.fixedBytes >= 8 && quality.fixedRatio >= 0.55;
    return quality;
}

QList<uint64_t> searchAobBuffer(const QByteArray& haystack, const AobPattern& pattern, uint64_t baseAddress) {
    QList<uint64_t> matches;
    if (!pattern.isValid() || haystack.size() < pattern.size()) return matches;

    const qsizetype patternSize = pattern.size();
    const qsizetype limit = haystack.size() - patternSize;
    const auto* data = reinterpret_cast<const uint8_t*>(haystack.constData());

    qsizetype anchorIndex = -1;
    for (qsizetype j = patternSize - 1; j >= 0; --j) {
        if (pattern.bytes[static_cast<size_t>(j)].has_value()) {
            anchorIndex = j;
            break;
        }
    }

    if (anchorIndex < 0) {
        for (qsizetype i = 0; i <= limit; ++i) {
            matches.append(baseAddress + static_cast<uint64_t>(i));
        }
        return matches;
    }

    std::array<qsizetype, 256> anchorMismatchShift;
    anchorMismatchShift.fill(anchorIndex + 1);
    for (qsizetype j = 0; j < anchorIndex; ++j) {
        const auto expected = pattern.bytes[static_cast<size_t>(j)];
        if (expected) {
            anchorMismatchShift[*expected] = anchorIndex - j;
        }
    }

    const uint8_t anchorByte = *pattern.bytes[static_cast<size_t>(anchorIndex)];
    qsizetype i = 0;
    while (i <= limit) {
        const uint8_t currentAnchor = data[i + anchorIndex];
        if (currentAnchor != anchorByte) {
            i += std::max<qsizetype>(1, anchorMismatchShift[currentAnchor]);
            continue;
        }

        bool ok = true;
        for (qsizetype j = 0; j < patternSize; ++j) {
            if (j == anchorIndex) {
                continue;
            }
            const auto expected = pattern.bytes[static_cast<size_t>(j)];
            if (expected && data[i + j] != *expected) {
                ok = false;
                break;
            }
        }
        if (ok) {
            matches.append(baseAddress + static_cast<uint64_t>(i));
        }
        ++i;
    }

    return matches;
}

AobScanResult scanAobPattern(const ProcessHandle& process, const AobPattern& pattern, const AobScanOptions& options) {
    AobScanResult result;
    if (!process.isValid()) {
        result.error = "Process handle invalide.";
        return result;
    }
    if (!pattern.isValid()) {
        result.error = pattern.error.isEmpty() ? "Pattern AOB invalide." : pattern.error;
        return result;
    }

    const int maxResults = std::clamp(options.maxResults, 1, 10000);
    const size_t chunkSize = std::clamp<size_t>(options.chunkSize, 64 * 1024, 16 * 1024 * 1024);
    const size_t overlap = pattern.bytes.size() > 1 ? pattern.bytes.size() - 1 : 0;
    const auto regions = MemoryMap::snapshot(process);
    MemoryReader reader(process);

    for (const auto& region : regions) {
        if (!regionMatchesAobOptions(region, options)) continue;

        uint64_t start = region.baseAddress;
        uint64_t end = region.baseAddress + region.size;
        if (options.startAddress > 0) start = std::max(start, options.startAddress);
        if (options.stopAddress > 0) end = std::min(end, options.stopAddress);
        if (start >= end) continue;

        ++result.regionsScanned;
        QByteArray previousTail;
        for (uint64_t cursor = start; cursor < end;) {
            const uint64_t remaining = end - cursor;
            const size_t toRead = static_cast<size_t>(std::min<uint64_t>(remaining, chunkSize));
            const auto read = reader.readChunked(cursor, toRead, 64 * 1024);
            if (!read.success && read.bytesRead == 0) {
                result.partial = true;
                break;
            }

            QByteArray scanData = previousTail;
            scanData.append(read.data);
            const uint64_t scanBase = cursor - static_cast<uint64_t>(previousTail.size());
            const auto found = searchAobBuffer(scanData, pattern, scanBase);
            for (uint64_t address : found) {
                if (address < cursor && !previousTail.isEmpty()) continue;
                AobMatch match;
                match.address = address;
                match.regionBase = region.baseAddress;
                match.regionSize = region.size;
                match.protection = region.protection;
                match.memoryType = region.type;
                result.matches.append(match);
                ++result.matchesFound;
                if (result.matches.size() >= maxResults) {
                    result.partial = true;
                    result.success = true;
                    return result;
                }
            }

            result.bytesScanned += static_cast<uint64_t>(read.bytesRead);
            if (overlap > 0) {
                const qsizetype tailSize = static_cast<qsizetype>(std::min<size_t>(overlap, static_cast<size_t>(read.data.size())));
                previousTail = read.data.right(tailSize);
            }

            if (read.bytesRead == 0) break;
            cursor += static_cast<uint64_t>(read.bytesRead);
            if (read.partial) {
                result.partial = true;
                break;
            }
        }
    }

    result.success = true;
    return result;
}

} // namespace killcore
