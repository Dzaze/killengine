#include "scanner/display_value_tracker.h"

#include "scanner/value_variants.h"

#include <QHash>
#include <QSet>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace killcore {

namespace {

bool asciiDigitByte(char byte) {
    return byte >= '0' && byte <= '9';
}

bool isUtf16Encoding(const QString& encoding) {
    return encoding.compare("utf16", Qt::CaseInsensitive) == 0
        || encoding.compare("utf16le", Qt::CaseInsensitive) == 0;
}

bool hasNumericBoundary(const QByteArray& haystack, qsizetype index, qsizetype length, const QString& encoding) {
    if (isUtf16Encoding(encoding)) {
        const qsizetype before = index - 2;
        if (before >= 0 &&
            haystack.at(before + 1) == '\0' &&
            asciiDigitByte(haystack.at(before))) {
            return false;
        }
        const qsizetype after = index + length;
        if (after + 1 < haystack.size() &&
            haystack.at(after + 1) == '\0' &&
            asciiDigitByte(haystack.at(after))) {
            return false;
        }
        return true;
    }

    if (index > 0 && asciiDigitByte(haystack.at(index - 1))) {
        return false;
    }
    const qsizetype after = index + length;
    if (after < haystack.size() && asciiDigitByte(haystack.at(after))) {
        return false;
    }
    return true;
}

QString variantKey(ValueType type, const QString& label) {
    return valueTypeToString(type) + "|" + label;
}

QHash<QString, QByteArray> variantBytesByKey(const QList<ValueVariant>& variants) {
    QHash<QString, QByteArray> bytes;
    for (const auto& variant : variants) {
        bytes.insert(variantKey(variant.value.type, variant.label), scanValueToBytes(variant.value));
    }
    return bytes;
}

double bytesToDouble(const QByteArray& bytes, ValueType type) {
    if (bytes.size() < static_cast<qsizetype>(valueTypeSize(type))) {
        return 0.0;
    }

    switch (type) {
        case ValueType::Int8: {
            int8_t value = 0;
            std::memcpy(&value, bytes.constData(), sizeof(value));
            return static_cast<double>(value);
        }
        case ValueType::UInt8: {
            uint8_t value = 0;
            std::memcpy(&value, bytes.constData(), sizeof(value));
            return static_cast<double>(value);
        }
        case ValueType::Int16: {
            int16_t value = 0;
            std::memcpy(&value, bytes.constData(), sizeof(value));
            return static_cast<double>(value);
        }
        case ValueType::UInt16: {
            uint16_t value = 0;
            std::memcpy(&value, bytes.constData(), sizeof(value));
            return static_cast<double>(value);
        }
        case ValueType::Int32: {
            int32_t value = 0;
            std::memcpy(&value, bytes.constData(), sizeof(value));
            return static_cast<double>(value);
        }
        case ValueType::UInt32: {
            uint32_t value = 0;
            std::memcpy(&value, bytes.constData(), sizeof(value));
            return static_cast<double>(value);
        }
        case ValueType::Int64: {
            int64_t value = 0;
            std::memcpy(&value, bytes.constData(), sizeof(value));
            return static_cast<double>(value);
        }
        case ValueType::UInt64: {
            uint64_t value = 0;
            std::memcpy(&value, bytes.constData(), sizeof(value));
            return static_cast<double>(value);
        }
        case ValueType::Float32: {
            float value = 0;
            std::memcpy(&value, bytes.constData(), sizeof(value));
            return static_cast<double>(value);
        }
        case ValueType::Float64: {
            double value = 0;
            std::memcpy(&value, bytes.constData(), sizeof(value));
            return value;
        }
    }
    return 0.0;
}

} // namespace

QByteArray encodeUiStringValue(const QString& value, const QString& encoding) {
    if (isUtf16Encoding(encoding)) {
        QByteArray bytes;
        bytes.reserve(value.size() * 2);
        for (const QChar ch : value) {
            const ushort code = ch.unicode();
            bytes.append(static_cast<char>(code & 0xff));
            bytes.append(static_cast<char>((code >> 8) & 0xff));
        }
        return bytes;
    }
    return value.toLatin1();
}

QList<UiStringMatch> findUiStringMatchesInBuffer(
    const QByteArray& buffer,
    const QString& value,
    bool scanAscii,
    bool scanUtf16,
    bool numericBoundary,
    int maxResults) {
    QList<QPair<QString, QByteArray>> patterns;
    if (scanAscii) patterns.append({"ascii", encodeUiStringValue(value, "ascii")});
    if (scanUtf16) patterns.append({"utf16", encodeUiStringValue(value, "utf16")});

    QList<UiStringMatch> matches;
    const int boundedMax = std::clamp(maxResults, 1, 100000);
    for (const auto& pattern : patterns) {
        if (pattern.second.isEmpty()) continue;
        qsizetype from = 0;
        while (matches.size() < boundedMax) {
            const qsizetype found = buffer.indexOf(pattern.second, from);
            if (found < 0) break;
            from = found + 1;
            if (numericBoundary && !hasNumericBoundary(buffer, found, pattern.second.size(), pattern.first)) {
                continue;
            }
            matches.append({static_cast<size_t>(found), pattern.first, static_cast<int>(pattern.second.size())});
        }
    }
    return matches;
}

QList<UiStringSourceHit> findUiStringSourcesInBuffer(
    const QByteArray& buffer,
    size_t stringOffset,
    int stringByteLength,
    const QString& value,
    int maxResults,
    int alignment,
    int radiusBytes) {
    QList<UiStringSourceHit> hits;
    const auto variants = generateScanVariants(value, ValueType::Int32, false);
    const int boundedMax = std::clamp(maxResults, 1, 100000);
    const int boundedAlignment = std::clamp(alignment, 1, 16);
    const int boundedRadius = std::max(1, radiusBytes);
    const size_t stringEnd = stringOffset + static_cast<size_t>(std::max(0, stringByteLength));
    QSet<QString> seen;

    for (const auto& variant : variants) {
        const QByteArray needle = scanValueToBytes(variant.value);
        if (needle.isEmpty() || needle.size() > buffer.size()) continue;

        qsizetype from = 0;
        while (hits.size() < boundedMax * 4) {
            const qsizetype found = buffer.indexOf(needle, from);
            if (found < 0) break;
            from = found + 1;
            const size_t offset = static_cast<size_t>(found);
            if (boundedAlignment > 1 && (offset % static_cast<size_t>(boundedAlignment)) != 0) continue;
            if (offset >= stringOffset && offset < stringEnd) continue;

            const QString key = QString::number(offset) + "|" + valueTypeToString(variant.value.type);
            if (seen.contains(key)) continue;
            seen.insert(key);

            const size_t distance = offset > stringOffset ? offset - stringOffset : stringOffset - offset;
            double score = 1.0;
            score -= std::min<double>(0.55, static_cast<double>(distance) / static_cast<double>(boundedRadius) * 0.55);
            if (variant.secondary) score -= 0.18;
            const auto type = variant.value.type;
            if (type == ValueType::Int32 || type == ValueType::UInt32) score += 0.08;
            if (type == ValueType::Int8 || type == ValueType::UInt8) score -= 0.2;
            score = std::clamp(score, 0.05, 1.0);

            hits.append({offset, type, variant.label, needle, bytesToDouble(needle, type), distance, score});
        }
    }

    std::sort(hits.begin(), hits.end(), [](const UiStringSourceHit& a, const UiStringSourceHit& b) {
        if (std::abs(a.confidence - b.confidence) > 0.000001) return a.confidence > b.confidence;
        return a.distanceBytes < b.distanceBytes;
    });
    while (hits.size() > boundedMax) {
        hits.removeLast();
    }
    return hits;
}

QByteArray targetBytesForTypeAndVariant(
    const QString& value,
    ValueType type,
    const QString& variantLabel,
    QString* error) {
    if (!variantLabel.trimmed().isEmpty()) {
        const auto variants = generateScanVariants(value, type, true);
        const auto bytes = variantBytesByKey(variants);
        const QString key = variantKey(type, variantLabel);
        if (bytes.contains(key)) {
            return bytes.value(key);
        }
    }

    ScanValue parsed;
    QString parseError;
    if (!parseScanValue(value, type, &parsed, &parseError)) {
        if (error) *error = parseError;
        return {};
    }
    return scanValueToBytes(parsed);
}

} // namespace killcore
