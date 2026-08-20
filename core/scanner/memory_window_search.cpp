#include "memory_window_search.h"

#include <algorithm>
#include <QSet>

namespace killcore {

QList<MemoryWindowMatch> findValuesInMemoryWindow(
    const QByteArray& buffer,
    uint64_t windowStart,
    uint64_t anchorAddress,
    int excludeBytes,
    const QString& value,
    int maxResults,
    int alignment) {
    QList<MemoryWindowMatch> hits;

    const auto variants = generateScanVariants(value, ValueType::Int32, false);
    const int boundedMax = std::clamp(maxResults, 1, 5000);
    const int boundedAlignment = std::clamp(alignment, 1, 16);
    const uint64_t excludeStart = anchorAddress;
    const uint64_t excludeEnd = anchorAddress + static_cast<uint64_t>(std::max(0, excludeBytes));
    QSet<QString> seen;

    for (const auto& variant : variants) {
        const QByteArray needle = scanValueToBytes(variant.value);
        if (needle.isEmpty() || needle.size() > buffer.size()) continue;

        qsizetype from = 0;
        while (hits.size() < boundedMax * 4) {
            const qsizetype found = buffer.indexOf(needle, from);
            if (found < 0) break;
            from = found + 1;

            const uint64_t address = windowStart + static_cast<uint64_t>(found);
            if (boundedAlignment > 1 && (address % static_cast<uint64_t>(boundedAlignment)) != 0) continue;
            if (address >= excludeStart && address < excludeEnd) continue;

            const QString key = QString::number(address) + "|" + valueTypeToString(variant.value.type);
            if (seen.contains(key)) continue;
            seen.insert(key);

            MemoryWindowMatch hit;
            hit.address = address;
            hit.type = variant.value.type;
            hit.variantLabel = variant.label;
            hit.bytes = needle;
            hit.valueNumber = scanBytesToDouble(needle, variant.value.type);
            hit.offsetFromAnchor = static_cast<int64_t>(address) - static_cast<int64_t>(anchorAddress);
            hit.distanceBytes = address > anchorAddress ? address - anchorAddress : anchorAddress - address;
            hits.append(hit);
        }
    }

    std::sort(hits.begin(), hits.end(), [](const MemoryWindowMatch& a, const MemoryWindowMatch& b) {
        return a.distanceBytes < b.distanceBytes;
    });
    while (hits.size() > boundedMax) {
        hits.removeLast();
    }
    return hits;
}

} // namespace killcore
