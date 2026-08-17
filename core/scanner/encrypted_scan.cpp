#include "encrypted_scan.h"

#include <algorithm>
#include <cstring>

namespace killcore {

uint64_t applyEncryption(uint64_t rawValue, EncryptedScanMode mode, uint64_t key) {
    switch (mode) {
        case EncryptedScanMode::XorKey:  return rawValue ^ key;
        case EncryptedScanMode::AddKey:  return rawValue + key;
        case EncryptedScanMode::SubKey:  return rawValue - key;
        case EncryptedScanMode::NotBits: return ~rawValue;
        default: return rawValue;
    }
}

uint64_t reverseEncryption(uint64_t displayValue, EncryptedScanMode mode, uint64_t key) {
    switch (mode) {
        case EncryptedScanMode::XorKey:  return displayValue ^ key;
        case EncryptedScanMode::AddKey:  return displayValue - key;
        case EncryptedScanMode::SubKey:  return displayValue + key;
        case EncryptedScanMode::NotBits: return ~displayValue;
        default: return displayValue;
    }
}

EncryptedScanResult scanEncryptedInBuffer(
    const QByteArray& buffer,
    uint64_t baseAddress,
    uint64_t displayValue,
    const EncryptedScanOptions& options) {

    EncryptedScanResult result;
    const size_t typeSize = valueTypeSize(options.valueType);
    if (typeSize == 0 || buffer.size() < static_cast<int>(typeSize)) {
        result.error = "Invalid type size or buffer too small";
        return result;
    }

    uint64_t typeMask = 0;
    switch (typeSize) {
        case 1: typeMask = 0xFF; break;
        case 2: typeMask = 0xFFFF; break;
        case 4: typeMask = 0xFFFFFFFF; break;
        case 8: typeMask = 0xFFFFFFFFFFFFFFFFULL; break;
        default: typeMask = 0xFFFFFFFF; break;
    }

    uint64_t keyStart = options.key;
    uint64_t keyEnd = options.key + 1;

    if (options.keySearchBits > 0) {
        if (options.keySearchBits > 32) {
            result.error = "keySearchBits above 32 is not supported for bounded brute-force";
            return result;
        }

        const uint64_t range = (options.keySearchBits == 64)
            ? 0
            : (1ULL << static_cast<uint32_t>(options.keySearchBits));
        keyStart = 0;
        keyEnd = std::min<uint64_t>(range, 0x100000ULL);
        if (range > keyEnd) {
            result.partial = true;
        }
    }

    const int maxOffset = buffer.size() - static_cast<int>(typeSize);
    const size_t matchLimit = (options.keySearchBits > 0) ? 100 : 10000;

    for (int offset = 0; offset <= maxOffset; ++offset) {
        uint64_t rawValue = 0;
        std::memcpy(&rawValue, buffer.constData() + offset, typeSize);
        rawValue &= typeMask;

        for (uint64_t key = keyStart; key < keyEnd; ++key) {
            const uint64_t transformed = applyEncryption(rawValue, options.mode, key) & typeMask;

            if (transformed == (displayValue & typeMask)) {
                ScanMatch match;
                match.address = baseAddress + static_cast<uint64_t>(offset);
                match.type = options.valueType;
                match.confidence = 0.8;
                match.variantLabel = QStringLiteral("Encrypted (%1 key=0x%2)")
                    .arg(options.mode == EncryptedScanMode::XorKey ? "XOR"
                       : options.mode == EncryptedScanMode::AddKey ? "ADD"
                       : options.mode == EncryptedScanMode::SubKey ? "SUB"
                       : "NOT")
                    .arg(key, 0, 16);
                result.matches.append(match);

                if (result.keyFound == 0) {
                    result.keyFound = key;
                }

                if (static_cast<size_t>(result.matches.size()) >= matchLimit) {
                    result.success = true;
                    result.partial = true;
                    return result;
                }
            }
        }
    }

    result.success = true;
    return result;
}

EncryptedScanResult scanGroupInBuffer(
    const QByteArray& buffer,
    uint64_t baseAddress,
    const GroupScanOptions& options) {

    EncryptedScanResult result;
    if (options.entries.isEmpty()) {
        result.error = "No entries in group scan";
        return result;
    }

    int64_t minOffset = options.entries[0].offset;
    int64_t maxOffset = minOffset;
    for (const auto& entry : options.entries) {
        minOffset = std::min(minOffset, entry.offset);
        maxOffset = std::max(maxOffset, entry.offset);
    }

    if (maxOffset - minOffset > options.maxDistance) {
        result.error = "Group entries span exceeds maxDistance";
        return result;
    }

    if (buffer.size() <= static_cast<int>(maxOffset - minOffset)) {
        result.error = "Buffer too small for group span";
        return result;
    }

    result.success = true;

    const int scanEnd = buffer.size() - static_cast<int>(maxOffset - minOffset);

    for (int baseOffset = 0; baseOffset < scanEnd; ++baseOffset) {
        bool allMatch = true;

        for (const auto& entry : options.entries) {
            const int entryOffset = baseOffset + static_cast<int>(entry.offset - minOffset);
            const size_t entrySize = valueTypeSize(entry.type);

            if (entryOffset + static_cast<int>(entrySize) > buffer.size()) {
                allMatch = false;
                break;
            }

            uint64_t rawValue = 0;
            std::memcpy(&rawValue, buffer.constData() + entryOffset, entrySize);

            bool matches = false;
            switch (entry.type) {
                case ValueType::Int8:
                    matches = (static_cast<int8_t>(rawValue & 0xFF) == entry.value.toInt());
                    break;
                case ValueType::UInt8:
                    matches = ((rawValue & 0xFF) == entry.value.toUInt());
                    break;
                case ValueType::Int16:
                    matches = (static_cast<int16_t>(rawValue & 0xFFFF) == entry.value.toInt());
                    break;
                case ValueType::UInt16:
                    matches = ((rawValue & 0xFFFF) == entry.value.toUInt());
                    break;
                case ValueType::Int32:
                    matches = (static_cast<int32_t>(rawValue & 0xFFFFFFFF) == entry.value.toInt());
                    break;
                case ValueType::UInt32:
                    matches = ((rawValue & 0xFFFFFFFF) == entry.value.toUInt());
                    break;
                case ValueType::Int64:
                    matches = (static_cast<int64_t>(rawValue) == entry.value.toLongLong());
                    break;
                case ValueType::UInt64:
                    matches = (rawValue == entry.value.toULongLong());
                    break;
                case ValueType::Float32: {
                    float v;
                    std::memcpy(&v, &rawValue, sizeof(float));
                    matches = (v == entry.value.toFloat());
                    break;
                }
                case ValueType::Float64: {
                    double v;
                    std::memcpy(&v, &rawValue, sizeof(double));
                    matches = (v == entry.value.toDouble());
                    break;
                }
            }

            if (!matches) {
                allMatch = false;
                break;
            }
        }

        if (allMatch) {
            ScanMatch match;
            match.address = baseAddress + static_cast<uint64_t>(baseOffset) - static_cast<uint64_t>(minOffset);
            match.type = options.entries[0].type;
            match.confidence = 0.95;
            match.variantLabel = QStringLiteral("Group (%1 values)").arg(options.entries.size());
            result.matches.append(match);

            if (static_cast<size_t>(result.matches.size()) >= options.maxResults) {
                result.partial = true;
                return result;
            }
        }
    }

    return result;
}

} // namespace killcore
