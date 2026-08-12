#include "scan_types.h"

#include <limits>
#include <cstring>

namespace killcore {

QString valueTypeToString(ValueType type) {
    switch (type) {
        case ValueType::Int8:    return "Int8";
        case ValueType::UInt8:   return "UInt8";
        case ValueType::Int16:   return "Int16";
        case ValueType::UInt16:  return "UInt16";
        case ValueType::Int32:   return "Int32";
        case ValueType::UInt32:  return "UInt32";
        case ValueType::Int64:   return "Int64";
        case ValueType::UInt64:  return "UInt64";
        case ValueType::Float32: return "Float32";
        case ValueType::Float64: return "Float64";
    }
    return "Unknown";
}

bool parseValueType(const QString& text, ValueType* out) {
    if (!out) return false;

    const QString normalized = text.trimmed().toLower();
    if (normalized == "int8" || normalized == "i8") {
        *out = ValueType::Int8;
        return true;
    }
    if (normalized == "uint8" || normalized == "u8" || normalized == "byte") {
        *out = ValueType::UInt8;
        return true;
    }
    if (normalized == "int16" || normalized == "i16" || normalized == "short") {
        *out = ValueType::Int16;
        return true;
    }
    if (normalized == "uint16" || normalized == "u16" || normalized == "ushort") {
        *out = ValueType::UInt16;
        return true;
    }
    if (normalized == "int32" || normalized == "i32") {
        *out = ValueType::Int32;
        return true;
    }
    if (normalized == "uint32" || normalized == "u32") {
        *out = ValueType::UInt32;
        return true;
    }
    if (normalized == "int64" || normalized == "i64") {
        *out = ValueType::Int64;
        return true;
    }
    if (normalized == "uint64" || normalized == "u64") {
        *out = ValueType::UInt64;
        return true;
    }
    if (normalized == "float32" || normalized == "float" || normalized == "f32") {
        *out = ValueType::Float32;
        return true;
    }
    if (normalized == "float64" || normalized == "double" || normalized == "f64") {
        *out = ValueType::Float64;
        return true;
    }

    return false;
}

bool parseScanValue(const QString& text, ValueType type, ScanValue* out, QString* error) {
    if (!out) return false;

    bool ok = false;
    const QString normalized = text.trimmed().replace(" ", "");
    ScanValue parsed;
    parsed.type = type;

    switch (type) {
        case ValueType::Int8: {
            const qlonglong value = normalized.toLongLong(&ok, 10);
            if (ok && value >= std::numeric_limits<int8_t>::min() && value <= std::numeric_limits<int8_t>::max()) {
                parsed.value = static_cast<int>(value);
            } else {
                ok = false;
            }
            break;
        }
        case ValueType::UInt8: {
            const qulonglong value = normalized.toULongLong(&ok, 10);
            if (ok && value <= std::numeric_limits<uint8_t>::max()) {
                parsed.value = static_cast<uint>(value);
            } else {
                ok = false;
            }
            break;
        }
        case ValueType::Int16: {
            const qlonglong value = normalized.toLongLong(&ok, 10);
            if (ok && value >= std::numeric_limits<int16_t>::min() && value <= std::numeric_limits<int16_t>::max()) {
                parsed.value = static_cast<int>(value);
            } else {
                ok = false;
            }
            break;
        }
        case ValueType::UInt16: {
            const qulonglong value = normalized.toULongLong(&ok, 10);
            if (ok && value <= std::numeric_limits<uint16_t>::max()) {
                parsed.value = static_cast<uint>(value);
            } else {
                ok = false;
            }
            break;
        }
        case ValueType::Int32: {
            const qlonglong value = normalized.toLongLong(&ok, 10);
            if (ok && value >= std::numeric_limits<int32_t>::min() && value <= std::numeric_limits<int32_t>::max()) {
                parsed.value = static_cast<int>(value);
            } else {
                ok = false;
            }
            break;
        }
        case ValueType::UInt32: {
            const qulonglong value = normalized.toULongLong(&ok, 10);
            if (ok && value <= std::numeric_limits<uint32_t>::max()) {
                parsed.value = static_cast<uint>(value);
            } else {
                ok = false;
            }
            break;
        }
        case ValueType::Int64:
            parsed.value = normalized.toLongLong(&ok, 10);
            break;
        case ValueType::UInt64:
            parsed.value = normalized.toULongLong(&ok, 10);
            break;
        case ValueType::Float32:
            parsed.value = normalized.toFloat(&ok);
            break;
        case ValueType::Float64:
            parsed.value = normalized.toDouble(&ok);
            break;
    }

    if (!ok) {
        if (error) {
            *error = QString("Impossible de parser '%1' comme %2.")
                         .arg(text, valueTypeToString(type));
        }
        return false;
    }

    *out = parsed;
    return true;
}

bool parseNextScanMode(const QString& text, NextScanMode* out) {
    if (!out) return false;

    const QString normalized = text.trimmed().toLower();
    if (normalized == "exact") {
        *out = NextScanMode::Exact;
        return true;
    }
    if (normalized == "changed") {
        *out = NextScanMode::Changed;
        return true;
    }
    if (normalized == "unchanged") {
        *out = NextScanMode::Unchanged;
        return true;
    }
    if (normalized == "increased") {
        *out = NextScanMode::Increased;
        return true;
    }
    if (normalized == "decreased") {
        *out = NextScanMode::Decreased;
        return true;
    }
    if (normalized == "delta") {
        *out = NextScanMode::Delta;
        return true;
    }

    return false;
}

size_t valueTypeSize(ValueType type) {
    switch (type) {
        case ValueType::Int8:    return sizeof(int8_t);
        case ValueType::UInt8:   return sizeof(uint8_t);
        case ValueType::Int16:   return sizeof(int16_t);
        case ValueType::UInt16:  return sizeof(uint16_t);
        case ValueType::Int32:   return sizeof(int32_t);
        case ValueType::UInt32:  return sizeof(uint32_t);
        case ValueType::Int64:   return sizeof(int64_t);
        case ValueType::UInt64:  return sizeof(uint64_t);
        case ValueType::Float32: return sizeof(float);
        case ValueType::Float64: return sizeof(double);
    }
    return 0;
}

QByteArray scanValueToBytes(const ScanValue& value) {
    QByteArray bytes;

    switch (value.type) {
        case ValueType::Int8: {
            const int8_t typed = static_cast<int8_t>(value.value.toInt());
            bytes.resize(sizeof(typed));
            std::memcpy(bytes.data(), &typed, sizeof(typed));
            break;
        }
        case ValueType::UInt8: {
            const uint8_t typed = static_cast<uint8_t>(value.value.toUInt());
            bytes.resize(sizeof(typed));
            std::memcpy(bytes.data(), &typed, sizeof(typed));
            break;
        }
        case ValueType::Int16: {
            const int16_t typed = static_cast<int16_t>(value.value.toInt());
            bytes.resize(sizeof(typed));
            std::memcpy(bytes.data(), &typed, sizeof(typed));
            break;
        }
        case ValueType::UInt16: {
            const uint16_t typed = static_cast<uint16_t>(value.value.toUInt());
            bytes.resize(sizeof(typed));
            std::memcpy(bytes.data(), &typed, sizeof(typed));
            break;
        }
        case ValueType::Int32: {
            const int32_t typed = static_cast<int32_t>(value.value.toInt());
            bytes.resize(sizeof(typed));
            std::memcpy(bytes.data(), &typed, sizeof(typed));
            break;
        }
        case ValueType::UInt32: {
            const uint32_t typed = static_cast<uint32_t>(value.value.toUInt());
            bytes.resize(sizeof(typed));
            std::memcpy(bytes.data(), &typed, sizeof(typed));
            break;
        }
        case ValueType::Int64: {
            const int64_t typed = static_cast<int64_t>(value.value.toLongLong());
            bytes.resize(sizeof(typed));
            std::memcpy(bytes.data(), &typed, sizeof(typed));
            break;
        }
        case ValueType::UInt64: {
            const uint64_t typed = static_cast<uint64_t>(value.value.toULongLong());
            bytes.resize(sizeof(typed));
            std::memcpy(bytes.data(), &typed, sizeof(typed));
            break;
        }
        case ValueType::Float32: {
            const float typed = value.value.toFloat();
            bytes.resize(sizeof(typed));
            std::memcpy(bytes.data(), &typed, sizeof(typed));
            break;
        }
        case ValueType::Float64: {
            const double typed = value.value.toDouble();
            bytes.resize(sizeof(typed));
            std::memcpy(bytes.data(), &typed, sizeof(typed));
            break;
        }
    }

    return bytes;
}

} // namespace killcore
