#include "display_string_investigator.h"

#include "memory/memory_map.h"
#include "memory/memory_reader.h"
#include "scanner/display_value_tracker.h"
#include "scanner/memory_window_search.h"
#include "scanner/value_variants.h"

#include <QDateTime>
#include <QElapsedTimer>
#include <QHash>
#include <QList>
#include <QSet>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <utility>

namespace killengine {

namespace {

uint64_t uiInvestigationHash(const QByteArray& bytes) {
    uint64_t hash = 1469598103934665603ull;
    for (const unsigned char byte : bytes) {
        hash ^= static_cast<uint64_t>(byte);
        hash *= 1099511628211ull;
    }
    return hash;
}

bool parseHexAddress(const QString& addressHex, uint64_t* address) {
    if (!address) {
        return false;
    }
    QString normalized = addressHex.trimmed();
    if (normalized.startsWith("0x", Qt::CaseInsensitive)) {
        normalized = normalized.mid(2);
    }
    bool ok = false;
    const uint64_t parsed = normalized.toULongLong(&ok, 16);
    if (!ok) {
        return false;
    }
    *address = parsed;
    return true;
}

bool regionMatchesScanOptions(const killcore::MemoryRegion& region, const killcore::ScanOptions& options) {
    if (!region.readable || region.guarded || region.size == 0) {
        return false;
    }
    if (options.writableOnly && !region.writable) {
        return false;
    }
    if (options.executableOnly && !region.executable) {
        return false;
    }
    if (options.copyOnWriteOnly && region.type != killcore::MemoryType::Mapped && region.type != killcore::MemoryType::Image) {
        return false;
    }
    return true;
}

QString uiStringAddress(uint64_t address) {
    return QStringLiteral("%1").arg(address, 0, 16).toUpper();
}

const killcore::MemoryRegion* findRegionContaining(
    const QList<killcore::MemoryRegion>& regions,
    uint64_t address) {
    for (const auto& region : regions) {
        if (address >= region.baseAddress && address < region.baseAddress + region.size) {
            return &region;
        }
    }
    return nullptr;
}

double bytesToDouble(const QByteArray& bytes, killcore::ValueType type) {
    if (bytes.size() < static_cast<qsizetype>(killcore::valueTypeSize(type))) {
        return 0.0;
    }
    switch (type) {
    case killcore::ValueType::Int16: {
        int16_t value = 0;
        std::memcpy(&value, bytes.constData(), sizeof(value));
        return static_cast<double>(value);
    }
    case killcore::ValueType::UInt16: {
        uint16_t value = 0;
        std::memcpy(&value, bytes.constData(), sizeof(value));
        return static_cast<double>(value);
    }
    case killcore::ValueType::Int32: {
        int32_t value = 0;
        std::memcpy(&value, bytes.constData(), sizeof(value));
        return static_cast<double>(value);
    }
    case killcore::ValueType::UInt32: {
        uint32_t value = 0;
        std::memcpy(&value, bytes.constData(), sizeof(value));
        return static_cast<double>(value);
    }
    case killcore::ValueType::Int64: {
        int64_t value = 0;
        std::memcpy(&value, bytes.constData(), sizeof(value));
        return static_cast<double>(value);
    }
    case killcore::ValueType::Float32: {
        float value = 0.0f;
        std::memcpy(&value, bytes.constData(), sizeof(value));
        return static_cast<double>(value);
    }
    case killcore::ValueType::Float64: {
        double value = 0.0;
        std::memcpy(&value, bytes.constData(), sizeof(value));
        return value;
    }
    }
    return 0.0;
}

bool bytesEqual(const QByteArray& a, const QByteArray& b, killcore::ValueType type) {
    if (type == killcore::ValueType::Float32 || type == killcore::ValueType::Float64) {
        return std::abs(bytesToDouble(a, type) - bytesToDouble(b, type)) < 0.000001;
    }
    return a.left(static_cast<qsizetype>(killcore::valueTypeSize(type)))
        == b.left(static_cast<qsizetype>(killcore::valueTypeSize(type)));
}

QString variantKey(killcore::ValueType type, const QString& label) {
    return killcore::valueTypeToString(type) + QStringLiteral("|") + label;
}

} // namespace

UiStringInvestigator::UiStringInvestigator(
    const killcore::ProcessHandle& handle,
    TelemetryCallback telemetry,
    ScanOptionsCallback scanOptions,
    ScanStartedCallback scanStarted,
    ScanProgressCallback scanProgress)
    : m_handle(handle),
      m_appendScanTelemetry(std::move(telemetry)),
      m_scanOptionsFromSettingsAndExpertOptions(std::move(scanOptions)),
      m_scanStarted(std::move(scanStarted)),
      m_scanProgress(std::move(scanProgress)) {
}

QVariantMap UiStringInvestigator::scanUiStrings(const QString& value, const QVariantMap& optionsMap) const {
    QVariantMap result;
    QVariantList matches;
    result["success"] = false;
    result["matches"] = matches;

    const QString needleText = value.trimmed();
    if (needleText.isEmpty()) {
        result["error"] = "Valeur texte vide.";
        m_appendScanTelemetry("ui_string_scan", {
            {"success", false},
            {"error", result.value("error")},
            {"value", value},
        });
        return result;
    }
    if (!m_handle.isValid()) {
        result["error"] = "Aucun processus attaché.";
        m_appendScanTelemetry("ui_string_scan", {
            {"success", false},
            {"error", result.value("error")},
            {"value", needleText},
        });
        return result;
    }

    killcore::ScanOptions options = m_scanOptionsFromSettingsAndExpertOptions(optionsMap);
    options.writableOnly = optionsMap.value("writableOnly", true).toBool();
    options.executableOnly = optionsMap.value("executableOnly", false).toBool();
    options.copyOnWriteOnly = optionsMap.value("copyOnWriteOnly", false).toBool();

    const bool scanAscii = optionsMap.value("ascii", true).toBool();
    const bool scanUtf16 = optionsMap.value("utf16", true).toBool();
    const bool numericBoundary = optionsMap.value("numericBoundary", true).toBool();
    const int maxResults = std::clamp(optionsMap.value("maxResults", 5000).toInt(), 1, 50000);
    constexpr size_t kChunkSize = 1024 * 1024;

    QList<QPair<QString, QByteArray>> patterns;
    if (scanAscii) {
        const QByteArray ascii = killcore::encodeUiStringValue(needleText, "ascii");
        if (!ascii.isEmpty()) {
            patterns.append({"ascii", ascii});
        }
    }
    if (scanUtf16) {
        const QByteArray utf16 = killcore::encodeUiStringValue(needleText, "utf16");
        if (!utf16.isEmpty()) {
            patterns.append({"utf16", utf16});
        }
    }
    if (patterns.isEmpty()) {
        result["error"] = "Aucun encodage texte sélectionné.";
        m_appendScanTelemetry("ui_string_scan", {
            {"success", false},
            {"error", result.value("error")},
            {"value", needleText},
            {"ascii", scanAscii},
            {"utf16", scanUtf16},
        });
        return result;
    }

    m_scanStarted();
    m_scanProgress(0);

    QElapsedTimer timer;
    timer.start();
    killcore::MemoryReader reader(m_handle);
    const auto regions = killcore::MemoryMap::snapshot(m_handle);
    uint64_t bytesScanned = 0;
    int regionsScanned = 0;
    bool partial = false;

    for (const auto& region : regions) {
        if (matches.size() >= maxResults) {
            partial = true;
            break;
        }
        if (!regionMatchesScanOptions(region, options)) {
            continue;
        }

        const uint64_t regionStart = region.baseAddress;
        const uint64_t regionEnd = region.baseAddress + region.size;
        const uint64_t effectiveStart = std::max(regionStart, options.startAddress);
        const uint64_t effectiveEnd = options.stopAddress == 0 ? regionEnd : std::min(regionEnd, options.stopAddress);
        if (effectiveEnd <= effectiveStart) {
            continue;
        }

        ++regionsScanned;
        const uint64_t regionSize = effectiveEnd - effectiveStart;
        size_t maxPatternSize = 1;
        for (const auto& pattern : patterns) {
            maxPatternSize = std::max(maxPatternSize, static_cast<size_t>(pattern.second.size()));
        }
        const size_t overlap = std::min<size_t>(maxPatternSize > 0 ? maxPatternSize - 1 : 0, 64);

        uint64_t offset = 0;
        QByteArray previousTail;
        while (offset < regionSize && matches.size() < maxResults) {
            const uint64_t remaining = regionSize - offset;
            const size_t toRead = static_cast<size_t>(std::min<uint64_t>(remaining, kChunkSize));
            const uint64_t readAddress = effectiveStart + offset;
            const auto read = reader.readChunked(readAddress, toRead, kChunkSize);
            if (!read.success && !read.partial) {
                break;
            }

            QByteArray buffer = previousTail + read.data;
            const uint64_t bufferBase = readAddress - static_cast<uint64_t>(previousTail.size());
            bytesScanned += read.bytesRead;

            const auto bufferMatches = killcore::findUiStringMatchesInBuffer(
                buffer,
                needleText,
                scanAscii,
                scanUtf16,
                numericBoundary,
                maxResults - matches.size());
            for (const auto& bufferMatch : bufferMatches) {
                QVariantMap match;
                const uint64_t matchAddress = bufferBase + static_cast<uint64_t>(bufferMatch.offset);
                const QByteArray bytes = killcore::encodeUiStringValue(needleText, bufferMatch.encoding);
                match["address"] = uiStringAddress(matchAddress);
                match["encoding"] = bufferMatch.encoding;
                match["text"] = needleText;
                match["byteLength"] = bufferMatch.byteLength;
                match["bytesHex"] = QString::fromLatin1(bytes.toHex(' ').toUpper());
                match["regionBase"] = uiStringAddress(region.baseAddress);
                match["regionSize"] = static_cast<qulonglong>(region.size);
                match["protection"] = killcore::protectionToString(region.protection);
                match["memoryType"] = killcore::memoryTypeToString(region.type);
                match["writable"] = region.writable;
                matches.append(match);
            }

            if (read.data.size() > static_cast<qsizetype>(overlap)) {
                previousTail = read.data.right(static_cast<qsizetype>(overlap));
            } else {
                previousTail = read.data;
            }
            offset += read.bytesRead;
            if (read.bytesRead == 0 || read.partial) {
                break;
            }
        }

        const int percent = regions.isEmpty()
            ? 100
            : std::clamp((regionsScanned * 100) / std::max(1, static_cast<int>(regions.size())), 0, 99);
        m_scanProgress(percent);
    }

    if (matches.size() >= maxResults) {
        partial = true;
    }

    m_scanProgress(100);

    result["success"] = true;
    result["partial"] = partial;
    result["matches"] = matches;
    result["matchesFound"] = matches.size();
    result["matchesReturned"] = matches.size();
    result["maxResults"] = maxResults;
    result["regionsScanned"] = regionsScanned;
    result["bytesScanned"] = static_cast<qulonglong>(bytesScanned);
    result["elapsedMs"] = static_cast<int>(timer.elapsed());
    result["writableOnly"] = options.writableOnly;
    result["error"] = "";
    m_appendScanTelemetry("ui_string_scan", {
        {"success", true},
        {"partial", partial},
        {"value", needleText},
        {"ascii", scanAscii},
        {"utf16", scanUtf16},
        {"numericBoundary", numericBoundary},
        {"writableOnly", options.writableOnly},
        {"copyOnWriteOnly", options.copyOnWriteOnly},
        {"executableOnly", options.executableOnly},
        {"maxResults", maxResults},
        {"matchesFound", matches.size()},
        {"matchesReturned", matches.size()},
        {"regionsScanned", regionsScanned},
        {"bytesScanned", static_cast<qulonglong>(bytesScanned)},
        {"elapsedMs", static_cast<int>(timer.elapsed())},
    });
    return result;
}

QVariantMap UiStringInvestigator::trackUiStringCandidates(const QVariantList& candidates, const QString& value) const {
    QVariantMap result;
    QVariantList survivors;
    result["success"] = false;
    result["survivors"] = survivors;
    QElapsedTimer timer;
    timer.start();

    const QString needleText = value.trimmed();
    if (needleText.isEmpty()) {
        result["error"] = "Nouvelle valeur texte vide.";
        m_appendScanTelemetry("ui_string_track", {
            {"success", false},
            {"error", result.value("error")},
            {"inputCandidates", candidates.size()},
            {"value", value},
        });
        return result;
    }
    if (!m_handle.isValid()) {
        result["error"] = "Aucun processus attaché.";
        m_appendScanTelemetry("ui_string_track", {
            {"success", false},
            {"error", result.value("error")},
            {"inputCandidates", candidates.size()},
            {"value", needleText},
        });
        return result;
    }

    killcore::MemoryReader reader(m_handle);
    const auto regions = killcore::MemoryMap::snapshot(m_handle);
    int checked = 0;
    int unreadable = 0;
    int moved = 0;
    QSet<QString> seenSurvivors;
    constexpr uint64_t kRescanRadius = 512;

    for (const auto& item : candidates) {
        const QVariantMap candidate = item.toMap();
        uint64_t address = 0;
        if (!parseHexAddress(candidate.value("address").toString(), &address)) {
            continue;
        }

        const QString encoding = candidate.value("encoding", "ascii").toString();
        const QByteArray expected = killcore::encodeUiStringValue(needleText, encoding);
        const int oldLength = std::max(0, candidate.value("byteLength", expected.size()).toInt());
        const int expectedSize = static_cast<int>(expected.size());
        const size_t readSize = static_cast<size_t>(std::clamp(
            std::max(oldLength, expectedSize),
            1,
            128));

        ++checked;
        const auto read = reader.read(address, readSize);
        if (!read.success && !read.partial) {
            ++unreadable;
            continue;
        }

        if (read.data.startsWith(expected)) {
            QVariantMap survivor = candidate;
            const QString survivorKey = encoding + "|" + uiStringAddress(address);
            if (seenSurvivors.contains(survivorKey)) {
                continue;
            }
            seenSurvivors.insert(survivorKey);
            survivor["text"] = needleText;
            survivor["byteLength"] = expected.size();
            survivor["bytesHex"] = QString::fromLatin1(expected.toHex(' ').toUpper());
            survivors.append(survivor);
            continue;
        }

        const killcore::MemoryRegion* region = findRegionContaining(regions, address);
        if (!region || !region->readable || region->guarded || region->size == 0) {
            continue;
        }

        const uint64_t regionStart = region->baseAddress;
        const uint64_t regionEnd = region->baseAddress + region->size;
        const uint64_t windowStart = address > kRescanRadius
            ? std::max(regionStart, address - kRescanRadius)
            : regionStart;
        const uint64_t windowEnd = std::min(regionEnd, address + static_cast<uint64_t>(readSize) + kRescanRadius);
        if (windowEnd <= windowStart) {
            continue;
        }

        const auto windowRead = reader.readChunked(
            windowStart,
            static_cast<size_t>(windowEnd - windowStart),
            4096);
        if (!windowRead.success && !windowRead.partial) {
            continue;
        }

        const bool scanAscii = encoding.compare("ascii", Qt::CaseInsensitive) == 0;
        const bool scanUtf16 = encoding.compare("utf16", Qt::CaseInsensitive) == 0
            || encoding.compare("utf16le", Qt::CaseInsensitive) == 0;
        const auto relocatedMatches = killcore::findUiStringMatchesInBuffer(
            windowRead.data,
            needleText,
            scanAscii,
            scanUtf16,
            /*numericBoundary=*/true,
            16);
        if (relocatedMatches.isEmpty()) {
            continue;
        }

        auto best = relocatedMatches.first();
        uint64_t bestAddress = windowStart + static_cast<uint64_t>(best.offset);
        uint64_t bestDistance = bestAddress > address ? bestAddress - address : address - bestAddress;
        for (const auto& match : relocatedMatches) {
            const uint64_t relocatedAddress = windowStart + static_cast<uint64_t>(match.offset);
            const uint64_t distance = relocatedAddress > address ? relocatedAddress - address : address - relocatedAddress;
            if (distance < bestDistance) {
                best = match;
                bestAddress = relocatedAddress;
                bestDistance = distance;
            }
        }

        const QString survivorKey = best.encoding + "|" + uiStringAddress(bestAddress);
        if (seenSurvivors.contains(survivorKey)) {
            continue;
        }
        seenSurvivors.insert(survivorKey);

        QVariantMap survivor = candidate;
        survivor["address"] = uiStringAddress(bestAddress);
        survivor["movedFrom"] = uiStringAddress(address);
        survivor["movedDistanceBytes"] = static_cast<qulonglong>(bestDistance);
        survivor["encoding"] = best.encoding;
        survivor["text"] = needleText;
        survivor["byteLength"] = best.byteLength;
        survivor["bytesHex"] = QString::fromLatin1(expected.toHex(' ').toUpper());
        survivors.append(survivor);
        ++moved;
    }

    result["success"] = true;
    result["checked"] = checked;
    result["unreadable"] = unreadable;
    result["moved"] = moved;
    result["remaining"] = survivors.size();
    result["survivors"] = survivors;
    result["error"] = "";
    m_appendScanTelemetry("ui_string_track", {
        {"success", true},
        {"value", needleText},
        {"inputCandidates", candidates.size()},
        {"checked", checked},
        {"unreadable", unreadable},
        {"moved", moved},
        {"remaining", survivors.size()},
        {"elapsedMs", static_cast<int>(timer.elapsed())},
    });
    return result;
}

QVariantMap UiStringInvestigator::analyzeUiStringSources(
    const QVariantMap& stringCandidate,
    const QString& value,
    const QVariantMap& optionsMap) const {
    QVariantMap result;
    QVariantList candidates;
    result["success"] = false;
    result["candidates"] = candidates;
    QElapsedTimer timer;
    timer.start();

    const QString rawValue = value.trimmed();
    if (rawValue.isEmpty()) {
        result["error"] = "Valeur source vide.";
        m_appendScanTelemetry("ui_string_sources_analyze", {
            {"success", false},
            {"error", result.value("error")},
            {"value", value},
        });
        return result;
    }
    if (!m_handle.isValid()) {
        result["error"] = "Aucun processus attaché.";
        m_appendScanTelemetry("ui_string_sources_analyze", {
            {"success", false},
            {"error", result.value("error")},
            {"value", rawValue},
        });
        return result;
    }

    uint64_t stringAddress = 0;
    if (!parseHexAddress(stringCandidate.value("address").toString(), &stringAddress)) {
        result["error"] = "Adresse string invalide.";
        m_appendScanTelemetry("ui_string_sources_analyze", {
            {"success", false},
            {"error", result.value("error")},
            {"value", rawValue},
            {"stringAddress", stringCandidate.value("address").toString()},
        });
        return result;
    }

    const int stringLength = std::clamp(stringCandidate.value("byteLength", 0).toInt(), 0, 256);
    const int radius = std::clamp(optionsMap.value("radiusBytes", 65536).toInt(), 256, 16 * 1024 * 1024);
    const int maxResults = std::clamp(optionsMap.value("maxResults", 200).toInt(), 1, 5000);
    const int alignment = std::clamp(optionsMap.value("alignment", 1).toInt(), 1, 16);

    const auto regions = killcore::MemoryMap::snapshot(m_handle);
    const killcore::MemoryRegion* region = findRegionContaining(regions, stringAddress);
    if (!region || !region->readable || region->guarded || region->size == 0) {
        result["error"] = "Région de la string illisible.";
        m_appendScanTelemetry("ui_string_sources_analyze", {
            {"success", false},
            {"error", result.value("error")},
            {"value", rawValue},
            {"stringAddress", uiStringAddress(stringAddress)},
            {"radiusBytes", radius},
        });
        return result;
    }

    const uint64_t regionStart = region->baseAddress;
    const uint64_t regionEnd = region->baseAddress + region->size;
    const uint64_t windowStart = stringAddress > static_cast<uint64_t>(radius)
        ? std::max(regionStart, stringAddress - static_cast<uint64_t>(radius))
        : regionStart;
    const uint64_t requestedEnd = stringAddress + static_cast<uint64_t>(stringLength) + static_cast<uint64_t>(radius);
    const uint64_t windowEnd = std::min(regionEnd, requestedEnd);
    if (windowEnd <= windowStart) {
        result["error"] = "Fenêtre d'analyse vide.";
        m_appendScanTelemetry("ui_string_sources_analyze", {
            {"success", false},
            {"error", result.value("error")},
            {"value", rawValue},
            {"stringAddress", uiStringAddress(stringAddress)},
            {"radiusBytes", radius},
            {"windowStart", uiStringAddress(windowStart)},
            {"windowEnd", uiStringAddress(windowEnd)},
        });
        return result;
    }

    const size_t readSize = static_cast<size_t>(std::min<uint64_t>(windowEnd - windowStart, 32ull * 1024ull * 1024ull));
    killcore::MemoryReader reader(m_handle);
    const auto read = reader.readChunked(windowStart, readSize, 64 * 1024);
    if (!read.success && !read.partial) {
        result["error"] = read.errorMessage;
        m_appendScanTelemetry("ui_string_sources_analyze", {
            {"success", false},
            {"error", result.value("error")},
            {"value", rawValue},
            {"stringAddress", uiStringAddress(stringAddress)},
            {"radiusBytes", radius},
            {"windowStart", uiStringAddress(windowStart)},
            {"windowEnd", uiStringAddress(windowEnd)},
            {"requestedBytes", static_cast<qulonglong>(readSize)},
        });
        return result;
    }

    const size_t stringOffset = stringAddress > windowStart
        ? static_cast<size_t>(stringAddress - windowStart)
        : 0;
    const auto hits = killcore::findUiStringSourcesInBuffer(
        read.data,
        stringOffset,
        stringLength,
        rawValue,
        maxResults,
        alignment,
        radius);

    for (const auto& hit : hits) {
        const uint64_t address = windowStart + static_cast<uint64_t>(hit.offset);
        QVariantMap entry;
        entry["address"] = uiStringAddress(address);
        entry["type"] = killcore::valueTypeToString(hit.type);
        entry["confidence"] = hit.confidence;
        entry["variantLabel"] = hit.variantLabel;
        entry["lastValueHex"] = QString::fromLatin1(hit.bytes.toHex(' ').toUpper());
        entry["lastValueNumber"] = hit.valueNumber;
        entry["distanceBytes"] = static_cast<qulonglong>(hit.distanceBytes);
        entry["offsetFromString"] = static_cast<qlonglong>(address) - static_cast<qlonglong>(stringAddress);
        entry["regionBase"] = uiStringAddress(region->baseAddress);
        entry["protection"] = killcore::protectionToString(region->protection);
        entry["memoryType"] = killcore::memoryTypeToString(region->type);
        candidates.append(entry);
    }

    result["success"] = true;
    result["partial"] = false;
    result["candidates"] = candidates;
    result["matchesFound"] = hits.size();
    result["matchesReturned"] = candidates.size();
    result["bytesScanned"] = static_cast<qulonglong>(read.bytesRead);
    result["windowStart"] = uiStringAddress(windowStart);
    result["windowEnd"] = uiStringAddress(windowStart + static_cast<uint64_t>(read.bytesRead));
    result["radiusBytes"] = radius;
    result["error"] = "";
    QVariantList samples;
    for (int i = 0; i < std::min<int>(candidates.size(), 10); ++i) {
        const QVariantMap candidate = candidates.at(i).toMap();
        samples.append(QVariantMap{
            {"address", candidate.value("address")},
            {"type", candidate.value("type")},
            {"variantLabel", candidate.value("variantLabel")},
            {"confidence", candidate.value("confidence")},
            {"distanceBytes", candidate.value("distanceBytes")},
            {"lastValueNumber", candidate.value("lastValueNumber")},
        });
    }
    m_appendScanTelemetry("ui_string_sources_analyze", {
        {"success", true},
        {"value", rawValue},
        {"stringAddress", uiStringAddress(stringAddress)},
        {"stringLength", stringLength},
        {"radiusBytes", radius},
        {"alignment", alignment},
        {"maxResults", maxResults},
        {"windowStart", uiStringAddress(windowStart)},
        {"windowEnd", result.value("windowEnd")},
        {"bytesScanned", static_cast<qulonglong>(read.bytesRead)},
        {"matchesFound", hits.size()},
        {"matchesReturned", candidates.size()},
        {"sampleCount", samples.size()},
        {"samples", samples},
        {"elapsedMs", static_cast<int>(timer.elapsed())},
    });
    return result;
}

QVariantMap UiStringInvestigator::scanMemoryWindow(
    const QString& addressHex,
    const QString& value,
    const QVariantMap& optionsMap) const {
    QVariantMap result;
    QVariantList candidates;
    result["success"] = false;
    result["candidates"] = candidates;
    QElapsedTimer timer;
    timer.start();

    const QString rawValue = value.trimmed();
    if (rawValue.isEmpty()) {
        result["error"] = "Valeur cible vide.";
        return result;
    }
    if (!m_handle.isValid()) {
        result["error"] = "Aucun processus attaché.";
        return result;
    }

    uint64_t anchorAddress = 0;
    if (!parseHexAddress(addressHex, &anchorAddress)) {
        result["error"] = "Adresse ancre invalide.";
        return result;
    }

    const int radius = std::clamp(optionsMap.value("radiusBytes", 65536).toInt(), 256, 16 * 1024 * 1024);
    const int maxResults = std::clamp(optionsMap.value("maxResults", 200).toInt(), 1, 5000);
    const int alignment = std::clamp(optionsMap.value("alignment", 1).toInt(), 1, 16);
    const int excludeBytes = std::clamp(optionsMap.value("excludeBytes", 8).toInt(), 0, 4096);

    const auto regions = killcore::MemoryMap::snapshot(m_handle);
    const killcore::MemoryRegion* region = findRegionContaining(regions, anchorAddress);
    if (!region || !region->readable || region->guarded || region->size == 0) {
        result["error"] = "Région autour de l'adresse ancre illisible.";
        return result;
    }

    const uint64_t regionStart = region->baseAddress;
    const uint64_t regionEnd = region->baseAddress + region->size;
    const uint64_t windowStart = anchorAddress > static_cast<uint64_t>(radius)
        ? std::max(regionStart, anchorAddress - static_cast<uint64_t>(radius))
        : regionStart;
    const uint64_t requestedEnd = anchorAddress + static_cast<uint64_t>(radius);
    const uint64_t windowEnd = std::min(regionEnd, requestedEnd);
    if (windowEnd <= windowStart) {
        result["error"] = "Fenêtre d'analyse vide.";
        return result;
    }

    const size_t readSize = static_cast<size_t>(std::min<uint64_t>(windowEnd - windowStart, 32ull * 1024ull * 1024ull));
    killcore::MemoryReader reader(m_handle);
    const auto read = reader.readChunked(windowStart, readSize, 64 * 1024);
    if (!read.success && !read.partial) {
        result["error"] = read.errorMessage;
        return result;
    }

    const auto hits = killcore::findValuesInMemoryWindow(
        read.data, windowStart, anchorAddress, excludeBytes, rawValue, maxResults, alignment);

    for (const auto& hit : hits) {
        QVariantMap entry;
        entry["address"] = uiStringAddress(hit.address);
        entry["type"] = killcore::valueTypeToString(hit.type);
        entry["variantLabel"] = hit.variantLabel;
        entry["lastValueHex"] = QString::fromLatin1(hit.bytes.toHex(' ').toUpper());
        entry["lastValueNumber"] = hit.valueNumber;
        entry["distanceBytes"] = static_cast<qulonglong>(hit.distanceBytes);
        entry["offsetFromAnchor"] = static_cast<qlonglong>(hit.offsetFromAnchor);
        entry["regionBase"] = uiStringAddress(region->baseAddress);
        entry["protection"] = killcore::protectionToString(region->protection);
        entry["memoryType"] = killcore::memoryTypeToString(region->type);
        candidates.append(entry);
    }

    result["success"] = true;
    result["candidates"] = candidates;
    result["matchesFound"] = hits.size();
    result["matchesReturned"] = candidates.size();
    result["bytesScanned"] = static_cast<qulonglong>(read.bytesRead);
    result["windowStart"] = uiStringAddress(windowStart);
    result["windowEnd"] = uiStringAddress(windowStart + static_cast<uint64_t>(read.bytesRead));
    result["radiusBytes"] = radius;
    result["error"] = "";
    m_appendScanTelemetry("memory_window_scan", {
        {"success", true},
        {"value", rawValue},
        {"anchorAddress", uiStringAddress(anchorAddress)},
        {"radiusBytes", radius},
        {"alignment", alignment},
        {"excludeBytes", excludeBytes},
        {"maxResults", maxResults},
        {"bytesScanned", static_cast<qulonglong>(read.bytesRead)},
        {"matchesFound", hits.size()},
        {"matchesReturned", candidates.size()},
        {"elapsedMs", static_cast<int>(timer.elapsed())},
    });
    return result;
}

QVariantMap UiStringInvestigator::trackUiStringSources(const QVariantList& sourceCandidates, const QString& value) const {
    QVariantMap result;
    QVariantList survivors;
    result["success"] = false;
    result["survivors"] = survivors;
    QElapsedTimer timer;
    timer.start();

    const QString rawValue = value.trimmed();
    if (rawValue.isEmpty()) {
        result["error"] = "Nouvelle valeur source vide.";
        m_appendScanTelemetry("ui_string_sources_track", {
            {"success", false},
            {"error", result.value("error")},
            {"inputCandidates", sourceCandidates.size()},
            {"value", value},
        });
        return result;
    }
    if (!m_handle.isValid()) {
        result["error"] = "Aucun processus attaché.";
        m_appendScanTelemetry("ui_string_sources_track", {
            {"success", false},
            {"error", result.value("error")},
            {"inputCandidates", sourceCandidates.size()},
            {"value", rawValue},
        });
        return result;
    }

    killcore::MemoryReader reader(m_handle);
    int checked = 0;
    int unreadable = 0;
    int incompatible = 0;

    for (const auto& item : sourceCandidates) {
        QVariantMap candidate = item.toMap();
        uint64_t address = 0;
        if (!parseHexAddress(candidate.value("address").toString(), &address)) {
            ++incompatible;
            continue;
        }

        killcore::ValueType type;
        if (!killcore::parseValueType(candidate.value("type").toString(), &type)) {
            ++incompatible;
            continue;
        }

        QString targetError;
        const QString variantLabel = candidate.value("variantLabel").toString();
        const QByteArray expected = killcore::targetBytesForTypeAndVariant(rawValue, type, variantLabel, &targetError);
        if (expected.isEmpty()) {
            ++incompatible;
            continue;
        }

        ++checked;
        const auto read = reader.read(address, killcore::valueTypeSize(type));
        if (!(read.success || read.partial) || read.bytesRead < killcore::valueTypeSize(type)) {
            ++unreadable;
            continue;
        }

        if (bytesEqual(read.data, expected, type)) {
            const double previousConfidence = candidate.value("confidence", 0.5).toDouble();
            const int previousHits = candidate.value("trackHits", 0).toInt();
            candidate["previousValueNumber"] = candidate.value("lastValueNumber");
            candidate["lastValueNumber"] = bytesToDouble(read.data, type);
            candidate["lastValueHex"] = QString::fromLatin1(read.data.left(static_cast<qsizetype>(expected.size())).toHex(' ').toUpper());
            candidate["expectedHex"] = QString::fromLatin1(expected.toHex(' ').toUpper());
            candidate["trackHits"] = previousHits + 1;
            candidate["confidence"] = std::clamp(previousConfidence + 0.12, 0.0, 1.0);
            survivors.append(candidate);
        }
    }

    result["success"] = true;
    result["checked"] = checked;
    result["unreadable"] = unreadable;
    result["incompatible"] = incompatible;
    result["remaining"] = survivors.size();
    result["survivors"] = survivors;
    result["error"] = "";
    QVariantList samples;
    for (int i = 0; i < std::min<int>(survivors.size(), 10); ++i) {
        const QVariantMap survivor = survivors.at(i).toMap();
        samples.append(QVariantMap{
            {"address", survivor.value("address")},
            {"type", survivor.value("type")},
            {"variantLabel", survivor.value("variantLabel")},
            {"confidence", survivor.value("confidence")},
            {"trackHits", survivor.value("trackHits")},
            {"lastValueNumber", survivor.value("lastValueNumber")},
        });
    }
    m_appendScanTelemetry("ui_string_sources_track", {
        {"success", true},
        {"value", rawValue},
        {"inputCandidates", sourceCandidates.size()},
        {"checked", checked},
        {"unreadable", unreadable},
        {"incompatible", incompatible},
        {"remaining", survivors.size()},
        {"sampleCount", samples.size()},
        {"samples", samples},
        {"elapsedMs", static_cast<int>(timer.elapsed())},
    });
    return result;
}

QVariantMap UiStringInvestigator::inspectUiStringOrigins(
    const QVariantList& stringCandidates,
    const QVariantMap& optionsMap) const {
    QVariantMap result;
    QVariantList targets;
    QVariantList pointerRefs;
    result["success"] = false;
    result["targets"] = targets;
    result["pointerRefs"] = pointerRefs;
    QElapsedTimer timer;
    timer.start();

    if (!m_handle.isValid()) {
        result["error"] = "Aucun processus attaché.";
        m_appendScanTelemetry("ui_string_origins_inspect", {
            {"success", false},
            {"error", result.value("error")},
            {"inputCandidates", stringCandidates.size()},
        });
        return result;
    }
    if (stringCandidates.isEmpty()) {
        result["error"] = "Aucune string à inspecter.";
        m_appendScanTelemetry("ui_string_origins_inspect", {
            {"success", false},
            {"error", result.value("error")},
            {"inputCandidates", stringCandidates.size()},
        });
        return result;
    }

    QList<uint64_t> addresses;
    addresses.reserve(stringCandidates.size());
    for (const auto& item : stringCandidates) {
        const QVariantMap candidate = item.toMap();
        uint64_t address = 0;
        if (parseHexAddress(candidate.value("address").toString(), &address)) {
            addresses.append(address);
        }
    }
    if (addresses.isEmpty()) {
        result["error"] = "Aucune adresse string valide.";
        m_appendScanTelemetry("ui_string_origins_inspect", {
            {"success", false},
            {"error", result.value("error")},
            {"inputCandidates", stringCandidates.size()},
        });
        return result;
    }
    std::sort(addresses.begin(), addresses.end());

    const uint64_t clusterStart = addresses.first();
    const uint64_t clusterEnd = addresses.last();
    const uint64_t clusterSpan = clusterEnd >= clusterStart ? clusterEnd - clusterStart : 0;
    const int maxRefs = std::clamp(optionsMap.value("maxRefs", 500).toInt(), 1, 5000);
    const uint64_t maxScanBytes = static_cast<uint64_t>(
        std::clamp(optionsMap.value("maxScanMb", 512).toInt(), 16, 4096)) * 1024ull * 1024ull;
    const bool writableOnly = optionsMap.value("writableOnly", true).toBool();
    constexpr size_t kChunkSize = 1024 * 1024;
    constexpr uint64_t kPointerSlack = 32;

    uint64_t commonStride = 0;
    if (addresses.size() >= 2) {
        commonStride = addresses.at(1) - addresses.at(0);
        for (qsizetype i = 2; i < addresses.size(); ++i) {
            const uint64_t delta = addresses.at(i) - addresses.at(i - 1);
            if (delta != commonStride) {
                commonStride = 0;
                break;
            }
        }
    }

    const auto regions = killcore::MemoryMap::snapshot(m_handle);
    const killcore::MemoryRegion* clusterRegion = findRegionContaining(regions, clusterStart);
    for (const uint64_t address : addresses) {
        QVariantMap target;
        target["address"] = uiStringAddress(address);
        target["offsetFromCluster"] = static_cast<qlonglong>(address - clusterStart);
        if (clusterRegion) {
            target["regionBase"] = uiStringAddress(clusterRegion->baseAddress);
            target["protection"] = killcore::protectionToString(clusterRegion->protection);
            target["memoryType"] = killcore::memoryTypeToString(clusterRegion->type);
        }
        targets.append(target);
    }

    killcore::MemoryReader reader(m_handle);
    uint64_t bytesScanned = 0;
    int regionsScanned = 0;
    bool partial = false;
    for (const auto& region : regions) {
        if (pointerRefs.size() >= maxRefs || bytesScanned >= maxScanBytes) {
            partial = true;
            break;
        }
        if (!region.readable || region.guarded || region.size < sizeof(uint64_t)) {
            continue;
        }
        if (writableOnly && !region.writable) {
            continue;
        }

        ++regionsScanned;
        uint64_t offset = 0;
        while (offset < region.size && pointerRefs.size() < maxRefs && bytesScanned < maxScanBytes) {
            const uint64_t remaining = region.size - offset;
            const size_t toRead = static_cast<size_t>(std::min<uint64_t>(
                remaining,
                std::min<uint64_t>(kChunkSize, maxScanBytes - bytesScanned)));
            const uint64_t readAddress = region.baseAddress + offset;
            const auto read = reader.readChunked(readAddress, toRead, kChunkSize);
            if (!read.success && !read.partial) {
                break;
            }
            bytesScanned += read.bytesRead;

            const qsizetype limit = read.data.size() - static_cast<qsizetype>(sizeof(uint64_t));
            for (qsizetype i = 0; i <= limit && pointerRefs.size() < maxRefs; i += 8) {
                uint64_t pointed = 0;
                std::memcpy(&pointed, read.data.constData() + i, sizeof(pointed));
                auto nearestIt = std::lower_bound(addresses.begin(), addresses.end(), pointed);
                uint64_t nearest = addresses.first();
                uint64_t nearestDistance = std::numeric_limits<uint64_t>::max();
                if (nearestIt != addresses.end()) {
                    nearest = *nearestIt;
                    nearestDistance = pointed > nearest ? pointed - nearest : nearest - pointed;
                }
                if (nearestIt != addresses.begin()) {
                    const uint64_t candidateAddress = *(nearestIt - 1);
                    const uint64_t distance = pointed > candidateAddress
                        ? pointed - candidateAddress
                        : candidateAddress - pointed;
                    if (distance < nearestDistance) {
                        nearest = candidateAddress;
                        nearestDistance = distance;
                    }
                }
                if (nearestDistance > kPointerSlack) {
                    continue;
                }

                QVariantMap ref;
                const uint64_t pointerAddress = readAddress + static_cast<uint64_t>(i);
                ref["address"] = uiStringAddress(pointerAddress);
                ref["pointsTo"] = uiStringAddress(pointed);
                ref["nearestString"] = uiStringAddress(nearest);
                ref["distanceToString"] = static_cast<qulonglong>(nearestDistance);
                ref["regionBase"] = uiStringAddress(region.baseAddress);
                ref["protection"] = killcore::protectionToString(region.protection);
                ref["memoryType"] = killcore::memoryTypeToString(region.type);
                ref["writable"] = region.writable;
                pointerRefs.append(ref);
            }

            offset += read.bytesRead;
            if (read.bytesRead == 0 || read.partial) {
                break;
            }
        }
    }

    result["success"] = true;
    result["targetCount"] = addresses.size();
    result["targets"] = targets;
    result["clusterStart"] = uiStringAddress(clusterStart);
    result["clusterEnd"] = uiStringAddress(clusterEnd);
    result["clusterSpanBytes"] = static_cast<qulonglong>(clusterSpan);
    result["commonStrideBytes"] = static_cast<qulonglong>(commonStride);
    result["pointerRefs"] = pointerRefs;
    result["pointerRefsFound"] = pointerRefs.size();
    result["bytesScanned"] = static_cast<qulonglong>(bytesScanned);
    result["regionsScanned"] = regionsScanned;
    result["partial"] = partial || pointerRefs.size() >= maxRefs;
    result["error"] = "";
    QVariantList refSamples;
    for (int i = 0; i < std::min<int>(pointerRefs.size(), 12); ++i) {
        const QVariantMap ref = pointerRefs.at(i).toMap();
        refSamples.append(QVariantMap{
            {"address", ref.value("address")},
            {"pointsTo", ref.value("pointsTo")},
            {"nearestString", ref.value("nearestString")},
            {"distanceToString", ref.value("distanceToString")},
            {"memoryType", ref.value("memoryType")},
            {"protection", ref.value("protection")},
            {"writable", ref.value("writable")},
        });
    }
    m_appendScanTelemetry("ui_string_origins_inspect", {
        {"success", true},
        {"inputCandidates", stringCandidates.size()},
        {"targetCount", addresses.size()},
        {"clusterStart", result.value("clusterStart")},
        {"clusterEnd", result.value("clusterEnd")},
        {"clusterSpanBytes", static_cast<qulonglong>(clusterSpan)},
        {"commonStrideBytes", static_cast<qulonglong>(commonStride)},
        {"maxRefs", maxRefs},
        {"maxScanBytes", static_cast<qulonglong>(maxScanBytes)},
        {"writableOnly", writableOnly},
        {"pointerSlackBytes", static_cast<qulonglong>(kPointerSlack)},
        {"pointerRefsFound", pointerRefs.size()},
        {"bytesScanned", static_cast<qulonglong>(bytesScanned)},
        {"regionsScanned", regionsScanned},
        {"partial", result.value("partial")},
        {"sampleCount", refSamples.size()},
        {"samples", refSamples},
        {"elapsedMs", static_cast<int>(timer.elapsed())},
    });
    return result;
}

QVariantMap UiStringInvestigator::startUiStringInvestigation(
    const QVariantList& stringCandidates,
    const QVariantList& sourceCandidates,
    const QVariantMap& optionsMap) {
    QVariantMap result;
    result["success"] = false;
    result["windows"] = 0;

    if (!m_handle.isValid()) {
        result["error"] = "Aucun processus attaché.";
        return result;
    }

    const int radius = std::clamp(optionsMap.value("radiusBytes", 4096).toInt(), 256, 1024 * 1024);
    const int maxWindows = std::clamp(optionsMap.value("maxWindows", 64).toInt(), 1, 512);
    const uint64_t maxBytes = static_cast<uint64_t>(
        std::clamp(optionsMap.value("maxBytesMb", 16).toInt(), 1, 256)) * 1024ull * 1024ull;
    const bool globalProbe = optionsMap.value("globalProbe", true).toBool();
    const int probeBlockSize = std::clamp(optionsMap.value("probeBlockSize", 64 * 1024).toInt(), 4096, 1024 * 1024);
    const uint64_t maxProbeBytes = static_cast<uint64_t>(
        std::clamp(optionsMap.value("maxProbeMb", 512).toInt(), 16, 2048)) * 1024ull * 1024ull;

    struct RequestWindow {
        uint64_t start{0};
        uint64_t end{0};
        QString label;
        QString reason;
    };
    QList<RequestWindow> requests;
    auto addRequest = [&](uint64_t address, int byteLength, const QString& label, const QString& reason) {
        const uint64_t extra = static_cast<uint64_t>(radius);
        const uint64_t start = address > extra ? address - extra : 0;
        const uint64_t valueEnd = address + static_cast<uint64_t>(std::max(1, byteLength));
        const uint64_t end = valueEnd > std::numeric_limits<uint64_t>::max() - extra
            ? std::numeric_limits<uint64_t>::max()
            : valueEnd + extra;
        requests.append({start, end, label, reason});
    };

    for (const auto& item : stringCandidates) {
        const QVariantMap candidate = item.toMap();
        uint64_t address = 0;
        if (parseHexAddress(candidate.value("address").toString(), &address)) {
            addRequest(
                address,
                candidate.value("byteLength", 8).toInt(),
                QString("string 0x%1").arg(uiStringAddress(address)),
                candidate.value("encoding", "string").toString());
        }
    }
    for (const auto& item : sourceCandidates) {
        const QVariantMap candidate = item.toMap();
        uint64_t address = 0;
        killcore::ValueType type = killcore::ValueType::Int32;
        if (parseHexAddress(candidate.value("address").toString(), &address)
            && killcore::parseValueType(candidate.value("type", "Int32").toString(), &type)) {
            const QString variant = candidate.value("variantLabel").toString();
            addRequest(
                address,
                static_cast<int>(killcore::valueTypeSize(type)),
                QString("source 0x%1").arg(uiStringAddress(address)),
                variant.isEmpty() ? killcore::valueTypeToString(type) : variant);
        }
    }

    if (requests.isEmpty()) {
        result["error"] = "Aucune string/source à observer.";
        return result;
    }

    std::sort(requests.begin(), requests.end(), [](const RequestWindow& a, const RequestWindow& b) {
        return a.start < b.start;
    });

    QList<RequestWindow> merged;
    for (const auto& req : requests) {
        if (merged.isEmpty() || req.start > merged.last().end + 4096) {
            merged.append(req);
            continue;
        }
        auto& last = merged.last();
        last.end = std::max(last.end, req.end);
        last.label += QString(", %1").arg(req.label);
        last.reason += QString(", %1").arg(req.reason);
    }

    killcore::MemoryReader reader(m_handle);
    auto& investigationWindows = m_windows;
    auto& investigationProbeBlocks = m_probeBlocks;
    investigationWindows.clear();
    investigationProbeBlocks.clear();
    uint64_t bytesCaptured = 0;
    uint64_t probeBytesCaptured = 0;
    int unreadable = 0;
    int probeUnreadable = 0;
    int probeRegions = 0;

    for (const auto& window : merged) {
        if (investigationWindows.size() >= maxWindows || bytesCaptured >= maxBytes) {
            break;
        }
        const uint64_t size64 = std::min<uint64_t>(window.end - window.start, maxBytes - bytesCaptured);
        if (size64 == 0) continue;
        const auto read = reader.readChunked(window.start, static_cast<size_t>(size64), 64 * 1024);
        if (!read.success && !read.partial) {
            ++unreadable;
            continue;
        }
        if (read.bytesRead == 0) {
            ++unreadable;
            continue;
        }
        investigationWindows.append({
            window.start,
            read.data.left(static_cast<qsizetype>(read.bytesRead)),
            window.label.left(240),
            window.reason.left(240),
        });
        bytesCaptured += read.bytesRead;
    }

    if (globalProbe) {
        const auto regions = killcore::MemoryMap::snapshot(m_handle);
        for (const auto& region : regions) {
            if (probeBytesCaptured >= maxProbeBytes) {
                break;
            }
            if (!region.readable || !region.writable || region.guarded || region.size == 0) {
                continue;
            }
            ++probeRegions;
            uint64_t offset = 0;
            while (offset < region.size && probeBytesCaptured < maxProbeBytes) {
                const uint64_t address = region.baseAddress + offset;
                const uint64_t remainingRegion = region.size - offset;
                const size_t toRead = static_cast<size_t>(std::min<uint64_t>({
                    static_cast<uint64_t>(probeBlockSize),
                    remainingRegion,
                    maxProbeBytes - probeBytesCaptured,
                }));
                if (toRead == 0) {
                    break;
                }
                const auto read = reader.read(address, toRead);
                if (!read.success && !read.partial) {
                    ++probeUnreadable;
                    offset += static_cast<uint64_t>(toRead);
                    continue;
                }
                if (read.bytesRead > 0) {
                    const QByteArray data = read.data.left(static_cast<qsizetype>(read.bytesRead));
                    investigationProbeBlocks.append({
                        address,
                        static_cast<int>(read.bytesRead),
                        uiInvestigationHash(data),
                        killcore::protectionToString(region.protection),
                        killcore::memoryTypeToString(region.type),
                    });
                    probeBytesCaptured += read.bytesRead;
                }
                offset += static_cast<uint64_t>(toRead);
            }
        }
    }

    m_startedMs = QDateTime::currentMSecsSinceEpoch();
    result["success"] = !investigationWindows.isEmpty();
    result["windows"] = investigationWindows.size();
    result["bytesCaptured"] = static_cast<qulonglong>(bytesCaptured);
    result["probeBlocks"] = investigationProbeBlocks.size();
    result["probeBytesCaptured"] = static_cast<qulonglong>(probeBytesCaptured);
    result["probeRegions"] = probeRegions;
    result["probeUnreadable"] = probeUnreadable;
    result["unreadable"] = unreadable;
    result["radiusBytes"] = radius;
    result["error"] = result.value("success").toBool() ? QString() : QString("Aucune fenêtre lisible capturée.");
    m_appendScanTelemetry("ui_string_investigation_start", {
        {"success", result.value("success")},
        {"stringCandidates", stringCandidates.size()},
        {"sourceCandidates", sourceCandidates.size()},
        {"requestWindows", requests.size()},
        {"mergedWindows", merged.size()},
        {"windows", result.value("windows")},
        {"bytesCaptured", result.value("bytesCaptured")},
        {"probeBlocks", result.value("probeBlocks")},
        {"probeBytesCaptured", result.value("probeBytesCaptured")},
        {"probeRegions", probeRegions},
        {"probeUnreadable", probeUnreadable},
        {"globalProbe", globalProbe},
        {"unreadable", unreadable},
        {"radiusBytes", radius},
        {"error", result.value("error")},
    });
    return result;
}

QVariantMap UiStringInvestigator::finishUiStringInvestigation(const QVariantMap& optionsMap) {
    QVariantMap result;
    QVariantList changes;
    result["success"] = false;
    result["changes"] = changes;

    if (!m_handle.isValid()) {
        result["error"] = "Aucun processus attaché.";
        return result;
    }
    auto& investigationWindows = m_windows;
    if (investigationWindows.isEmpty()) {
        result["error"] = "Aucune enquête live active.";
        return result;
    }

    const int maxChanges = std::clamp(optionsMap.value("maxChanges", 500).toInt(), 1, 5000);
    const int maxGlobalValueHits = std::clamp(optionsMap.value("maxGlobalValueHits", 250).toInt(), 0, 2000);
    const QString globalValue = optionsMap.value("value").toString().trimmed();
    killcore::MemoryReader reader(m_handle);
    int windowsChecked = 0;
    int unreadable = 0;
    int changedBytes = 0;
    bool partial = false;

    for (const auto& window : investigationWindows) {
        if (changes.size() >= maxChanges) {
            partial = true;
            break;
        }
        ++windowsChecked;
        const auto read = reader.readChunked(window.base, static_cast<size_t>(window.before.size()), 64 * 1024);
        if (!read.success && !read.partial) {
            ++unreadable;
            continue;
        }
        const qsizetype comparable = std::min(window.before.size(), read.data.size());
        qsizetype i = 0;
        while (i < comparable && changes.size() < maxChanges) {
            if (window.before.at(i) == read.data.at(i)) {
                ++i;
                continue;
            }
            const qsizetype start = i;
            while (i < comparable && window.before.at(i) != read.data.at(i)) {
                ++i;
            }
            const qsizetype length = i - start;
            changedBytes += static_cast<int>(length);
            const uint64_t address = window.base + static_cast<uint64_t>(start);
            QVariantMap change;
            change["address"] = uiStringAddress(address);
            change["offset"] = static_cast<qulonglong>(start);
            change["length"] = static_cast<int>(length);
            change["label"] = window.label;
            change["reason"] = window.reason;
            change["beforeHex"] = QString::fromLatin1(window.before.mid(start, std::min<qsizetype>(length, 16)).toHex(' ').toUpper());
            change["afterHex"] = QString::fromLatin1(read.data.mid(start, std::min<qsizetype>(length, 16)).toHex(' ').toUpper());
            if (start + 4 <= read.data.size() && start + 4 <= window.before.size()) {
                change["beforeInt32"] = bytesToDouble(window.before.mid(start, 4), killcore::ValueType::Int32);
                change["afterInt32"] = bytesToDouble(read.data.mid(start, 4), killcore::ValueType::Int32);
                change["beforeFloat32"] = bytesToDouble(window.before.mid(start, 4), killcore::ValueType::Float32);
                change["afterFloat32"] = bytesToDouble(read.data.mid(start, 4), killcore::ValueType::Float32);
            }
            changes.append(change);
        }
    }

    QVariantList globalValueHits;
    int probeBlocksChecked = 0;
    int probeBlocksChanged = 0;
    uint64_t probeBytesChecked = 0;
    uint64_t probeChangedBytes = 0;
    int probeUnreadable = 0;
    QSet<QString> globalSeen;
    const auto variants = globalValue.isEmpty()
        ? QList<killcore::ValueVariant>{}
        : killcore::generateScanVariants(globalValue, killcore::ValueType::Int32, false);
    auto& investigationProbeBlocks = m_probeBlocks;
    for (const auto& block : investigationProbeBlocks) {
        if (globalValueHits.size() >= maxGlobalValueHits) {
            partial = true;
            break;
        }
        ++probeBlocksChecked;
        const auto read = reader.read(block.base, static_cast<size_t>(block.size));
        if (!read.success && !read.partial) {
            ++probeUnreadable;
            continue;
        }
        if (read.bytesRead == 0) {
            ++probeUnreadable;
            continue;
        }
        probeBytesChecked += read.bytesRead;
        const QByteArray data = read.data.left(static_cast<qsizetype>(read.bytesRead));
        const uint64_t currentHash = uiInvestigationHash(data);
        if (currentHash == block.hash) {
            continue;
        }
        ++probeBlocksChanged;
        probeChangedBytes += read.bytesRead;

        for (const auto& variant : variants) {
            if (globalValueHits.size() >= maxGlobalValueHits) {
                partial = true;
                break;
            }
            const QByteArray needle = killcore::scanValueToBytes(variant.value);
            if (needle.isEmpty() || needle.size() > data.size()) {
                continue;
            }
            qsizetype from = 0;
            while (globalValueHits.size() < maxGlobalValueHits) {
                const qsizetype found = data.indexOf(needle, from);
                if (found < 0) {
                    break;
                }
                from = found + 1;
                const uint64_t address = block.base + static_cast<uint64_t>(found);
                const QString key = uiStringAddress(address) + "|" + killcore::valueTypeToString(variant.value.type) + "|" + variant.label;
                if (globalSeen.contains(key)) {
                    continue;
                }
                globalSeen.insert(key);

                QVariantMap hit;
                hit["address"] = uiStringAddress(address);
                hit["type"] = killcore::valueTypeToString(variant.value.type);
                hit["variantLabel"] = variant.label;
                hit["confidence"] = variant.secondary ? 0.62 : 0.74;
                hit["lastValueHex"] = QString::fromLatin1(needle.toHex(' ').toUpper());
                hit["lastValueNumber"] = bytesToDouble(needle, variant.value.type);
                hit["distanceBytes"] = static_cast<qulonglong>(0);
                hit["offsetFromString"] = static_cast<qlonglong>(0);
                hit["regionBase"] = uiStringAddress(block.base);
                hit["protection"] = block.protection;
                hit["memoryType"] = block.memoryType;
                hit["origin"] = "global_diff";
                globalValueHits.append(hit);
            }
        }
    }

    const qint64 elapsedMs = m_startedMs > 0
        ? QDateTime::currentMSecsSinceEpoch() - m_startedMs
        : 0;
    const int capturedWindows = investigationWindows.size();
    investigationWindows.clear();
    const int capturedProbeBlocks = investigationProbeBlocks.size();
    investigationProbeBlocks.clear();
    m_startedMs = 0;

    result["success"] = true;
    result["windowsChecked"] = windowsChecked;
    result["capturedWindows"] = capturedWindows;
    result["unreadable"] = unreadable;
    result["changedBytes"] = changedBytes;
    result["changesFound"] = changes.size();
    result["globalValueHits"] = globalValueHits;
    result["globalValueHitsFound"] = globalValueHits.size();
    result["probeBlocksCaptured"] = capturedProbeBlocks;
    result["probeBlocksChecked"] = probeBlocksChecked;
    result["probeBlocksChanged"] = probeBlocksChanged;
    result["probeBytesChecked"] = static_cast<qulonglong>(probeBytesChecked);
    result["probeChangedBytes"] = static_cast<qulonglong>(probeChangedBytes);
    result["probeUnreadable"] = probeUnreadable;
    result["partial"] = partial || changes.size() >= maxChanges;
    result["elapsedMs"] = static_cast<int>(elapsedMs);
    result["changes"] = changes;
    result["error"] = "";
    QVariantList samples;
    for (int idx = 0; idx < std::min<int>(changes.size(), 20); ++idx) {
        samples.append(changes.at(idx));
    }
    m_appendScanTelemetry("ui_string_investigation_finish", {
        {"success", true},
        {"capturedWindows", capturedWindows},
        {"windowsChecked", windowsChecked},
        {"unreadable", unreadable},
        {"changedBytes", changedBytes},
        {"changesFound", changes.size()},
        {"globalValue", globalValue},
        {"globalValueHitsFound", globalValueHits.size()},
        {"probeBlocksCaptured", capturedProbeBlocks},
        {"probeBlocksChecked", probeBlocksChecked},
        {"probeBlocksChanged", probeBlocksChanged},
        {"probeBytesChecked", static_cast<qulonglong>(probeBytesChecked)},
        {"probeChangedBytes", static_cast<qulonglong>(probeChangedBytes)},
        {"probeUnreadable", probeUnreadable},
        {"partial", result.value("partial")},
        {"elapsedMs", result.value("elapsedMs")},
        {"sampleCount", samples.size()},
        {"samples", samples},
    });
    return result;
}

QVariantMap UiStringInvestigator::startChangedPagesDiff(const QVariantMap& options) {
    QVariantMap result;
    result["success"] = false;

    if (!m_handle.isValid()) {
        result["error"] = "Aucun processus attaché.";
        return result;
    }

    const uint64_t maxBytes = static_cast<uint64_t>(
        std::clamp(options.value("maxBytesMb", 64).toInt(), 8, 256)) * 1024ull * 1024ull;
    const int blockSize = std::clamp(options.value("blockSize", 64 * 1024).toInt(), 4096, 1024 * 1024);
    const bool privateOnly = options.value("privateOnly", true).toBool();
    const bool writableOnly = options.value("writableOnly", true).toBool();

    auto& blocks = m_changedPagesDiffBlocks;
    blocks.clear();

    killcore::MemoryReader reader(m_handle);
    const auto regions = killcore::MemoryMap::snapshot(m_handle);
    uint64_t bytesCaptured = 0;
    int regionsScanned = 0;
    int unreadable = 0;
    bool partial = false;

    for (const auto& region : regions) {
        if (bytesCaptured >= maxBytes) {
            partial = true;
            break;
        }
        if (!region.readable || region.guarded || region.size == 0) {
            continue;
        }
        if (writableOnly && !region.writable) {
            continue;
        }
        if (privateOnly && region.type != killcore::MemoryType::Private) {
            continue;
        }
        ++regionsScanned;
        uint64_t offset = 0;
        while (offset < region.size && bytesCaptured < maxBytes) {
            const uint64_t address = region.baseAddress + offset;
            const size_t toRead = static_cast<size_t>(std::min<uint64_t>({
                static_cast<uint64_t>(blockSize),
                region.size - offset,
                maxBytes - bytesCaptured,
            }));
            if (toRead == 0) {
                break;
            }
            const auto read = reader.read(address, toRead);
            if (!read.success && !read.partial) {
                ++unreadable;
                offset += static_cast<uint64_t>(toRead);
                continue;
            }
            if (read.bytesRead > 0) {
                const QByteArray data = read.data.left(static_cast<qsizetype>(read.bytesRead));
                blocks.append({
                    address,
                    data,
                    uiInvestigationHash(data),
                    killcore::protectionToString(region.protection),
                    killcore::memoryTypeToString(region.type),
                });
                bytesCaptured += read.bytesRead;
            }
            offset += static_cast<uint64_t>(toRead);
        }
    }

    result["success"] = !blocks.isEmpty();
    result["blocksCaptured"] = blocks.size();
    result["bytesCaptured"] = static_cast<qulonglong>(bytesCaptured);
    result["regionsScanned"] = regionsScanned;
    result["unreadable"] = unreadable;
    result["partial"] = partial;
    result["maxBytes"] = static_cast<qulonglong>(maxBytes);
    result["blockSize"] = blockSize;
    result["privateOnly"] = privateOnly;
    result["writableOnly"] = writableOnly;
    result["error"] = blocks.isEmpty() ? "Aucun bloc private/RW lisible capturé." : QString();
    m_appendScanTelemetry("changed_pages_diff_start", result);
    return result;
}

QVariantMap UiStringInvestigator::finishChangedPagesDiff(
    const QString& previousValue,
    const QString& currentValue,
    const QVariantMap& options) {
    QVariantMap result;
    QVariantList hits;
    result["success"] = false;
    result["hits"] = hits;

    if (!m_handle.isValid()) {
        result["error"] = "Aucun processus attaché.";
        return result;
    }

    auto& blocks = m_changedPagesDiffBlocks;
    if (blocks.isEmpty()) {
        result["error"] = "Aucun diff de pages actif.";
        return result;
    }

    const QString prev = previousValue.trimmed();
    const QString cur = currentValue.trimmed();
    if (prev.isEmpty() || cur.isEmpty()) {
        result["error"] = "Valeurs précédente/actuelle requises.";
        return result;
    }

    const int maxHits = std::clamp(options.value("maxHits", 300).toInt(), 1, 2000);
    const int maxDistanceToChange = std::clamp(options.value("maxDistanceToChange", 256).toInt(), 0, 4096);
    const auto previousVariants = killcore::generateScanVariants(prev, killcore::ValueType::Int32, false);
    const auto currentVariants = killcore::generateScanVariants(cur, killcore::ValueType::Int32, false);
    QHash<QString, QByteArray> previousByKey;
    for (const auto& variant : previousVariants) {
        previousByKey.insert(variantKey(variant.value.type, variant.label), killcore::scanValueToBytes(variant.value));
    }

    auto distanceToChangedRanges = [](qsizetype offset, const QList<QPair<qsizetype, qsizetype>>& ranges) {
        qsizetype best = std::numeric_limits<qsizetype>::max();
        for (const auto& range : ranges) {
            if (offset >= range.first && offset < range.second) {
                return qsizetype{0};
            }
            const qsizetype distance = offset < range.first ? range.first - offset : offset - range.second;
            best = std::min(best, distance);
        }
        return best;
    };

    killcore::MemoryReader reader(m_handle);
    int blocksChecked = 0;
    int blocksChanged = 0;
    int unreadable = 0;
    uint64_t bytesChecked = 0;
    uint64_t changedBytes = 0;
    QSet<QString> seen;

    for (const auto& block : blocks) {
        if (hits.size() >= maxHits) {
            break;
        }
        ++blocksChecked;
        const auto read = reader.read(block.base, static_cast<size_t>(block.before.size()));
        if (!read.success && !read.partial) {
            ++unreadable;
            continue;
        }
        if (read.bytesRead == 0) {
            ++unreadable;
            continue;
        }
        const QByteArray after = read.data.left(static_cast<qsizetype>(read.bytesRead));
        bytesChecked += read.bytesRead;
        if (uiInvestigationHash(after) == block.hash) {
            continue;
        }
        ++blocksChanged;

        QList<QPair<qsizetype, qsizetype>> ranges;
        const qsizetype comparable = std::min(block.before.size(), after.size());
        qsizetype i = 0;
        while (i < comparable) {
            if (block.before.at(i) == after.at(i)) {
                ++i;
                continue;
            }
            const qsizetype start = i;
            while (i < comparable && block.before.at(i) != after.at(i)) {
                ++i;
            }
            ranges.append({start, i});
            changedBytes += static_cast<uint64_t>(i - start);
        }

        for (const auto& variant : currentVariants) {
            if (hits.size() >= maxHits) {
                break;
            }
            const QByteArray currentBytes = killcore::scanValueToBytes(variant.value);
            if (currentBytes.isEmpty() || currentBytes.size() > after.size()) {
                continue;
            }
            const QString keyBase = variantKey(variant.value.type, variant.label);
            const QByteArray previousBytes = previousByKey.value(keyBase);
            qsizetype from = 0;
            while (hits.size() < maxHits) {
                const qsizetype found = after.indexOf(currentBytes, from);
                if (found < 0) {
                    break;
                }
                from = found + 1;
                const qsizetype distance = distanceToChangedRanges(found, ranges);
                if (distance > maxDistanceToChange) {
                    continue;
                }
                const uint64_t address = block.base + static_cast<uint64_t>(found);
                const QString seenKey = uiStringAddress(address) + "|" + keyBase;
                if (seen.contains(seenKey)) {
                    continue;
                }
                seen.insert(seenKey);

                const bool sameOffsetOldValue =
                    !previousBytes.isEmpty()
                    && found + previousBytes.size() <= block.before.size()
                    && block.before.mid(found, previousBytes.size()) == previousBytes;
                QVariantMap hit;
                hit["address"] = uiStringAddress(address);
                hit["type"] = killcore::valueTypeToString(variant.value.type);
                hit["variantLabel"] = variant.label;
                hit["previousValue"] = prev;
                hit["currentValue"] = cur;
                hit["lastValueHex"] = QString::fromLatin1(currentBytes.toHex(' ').toUpper());
                hit["lastValueNumber"] = bytesToDouble(currentBytes, variant.value.type);
                hit["previousAtSameOffset"] = sameOffsetOldValue;
                hit["distanceToChangedBytes"] = static_cast<qulonglong>(distance);
                hit["confidence"] = sameOffsetOldValue ? 0.92 : (distance == 0 ? 0.78 : 0.62);
                hit["origin"] = sameOffsetOldValue ? "changed_pages_diff_old_to_new" : "changed_pages_diff_near_change";
                hit["regionBase"] = uiStringAddress(block.base);
                hit["protection"] = block.protection;
                hit["memoryType"] = block.memoryType;
                hits.append(hit);
            }
        }
    }

    const int capturedBlocks = blocks.size();
    blocks.clear();

    result["success"] = true;
    result["previousValue"] = prev;
    result["currentValue"] = cur;
    result["hits"] = hits;
    result["hitsFound"] = hits.size();
    result["capturedBlocks"] = capturedBlocks;
    result["blocksChecked"] = blocksChecked;
    result["blocksChanged"] = blocksChanged;
    result["bytesChecked"] = static_cast<qulonglong>(bytesChecked);
    result["changedBytes"] = static_cast<qulonglong>(changedBytes);
    result["unreadable"] = unreadable;
    result["partial"] = hits.size() >= maxHits;
    result["error"] = "";
    m_appendScanTelemetry("changed_pages_diff_finish", result);
    return result;
}


} // namespace killengine
