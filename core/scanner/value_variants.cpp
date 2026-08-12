#include "value_variants.h"

#include <limits>
#include <cmath>
#include <QSet>

namespace killcore {

namespace {

bool parseNumber(const QString& raw, double* out) {
    if (!out) return false;
    bool ok = false;
    const double value = raw.trimmed().replace(',', '.').toDouble(&ok);
    if (!ok) return false;
    *out = value;
    return true;
}

bool fitsInt32(double v) {
    return v >= std::numeric_limits<int32_t>::min()
        && v <= std::numeric_limits<int32_t>::max()
        && std::floor(v) == v;
}

bool fitsInt8(double v) {
    return v >= std::numeric_limits<int8_t>::min()
        && v <= std::numeric_limits<int8_t>::max()
        && std::floor(v) == v;
}

bool fitsInt16(double v) {
    return v >= std::numeric_limits<int16_t>::min()
        && v <= std::numeric_limits<int16_t>::max()
        && std::floor(v) == v;
}

bool fitsInt64(double v) {
    return v >= static_cast<double>(std::numeric_limits<int64_t>::min())
        && v <= static_cast<double>(std::numeric_limits<int64_t>::max())
        && std::floor(v) == v;
}

bool fitsUint8(double v) {
    return v >= 0.0
        && v <= static_cast<double>(std::numeric_limits<uint8_t>::max())
        && std::floor(v) == v;
}

bool fitsUint16(double v) {
    return v >= 0.0
        && v <= static_cast<double>(std::numeric_limits<uint16_t>::max())
        && std::floor(v) == v;
}

bool fitsUint32(double v) {
    return v >= 0.0
        && v <= static_cast<double>(std::numeric_limits<uint32_t>::max())
        && std::floor(v) == v;
}

bool fitsUint64(double v) {
    return v >= 0.0
        && v <= static_cast<double>(std::numeric_limits<uint64_t>::max())
        && std::floor(v) == v;
}

ScanValue makeScanValue(ValueType type, double raw) {
    ScanValue v;
    v.type = type;
    switch (type) {
        case ValueType::Int8:    v.value = static_cast<int8_t>(raw); break;
        case ValueType::UInt8:   v.value = static_cast<uint>(raw); break;
        case ValueType::Int16:   v.value = static_cast<int16_t>(raw); break;
        case ValueType::UInt16:  v.value = static_cast<uint>(raw); break;
        case ValueType::Int32:   v.value = static_cast<int32_t>(raw); break;
        case ValueType::UInt32:  v.value = static_cast<uint>(raw); break;
        case ValueType::Int64:   v.value = static_cast<int64_t>(raw); break;
        case ValueType::UInt64:  v.value = static_cast<qulonglong>(raw); break;
        case ValueType::Float32: v.value = static_cast<float>(raw); break;
        case ValueType::Float64: v.value = raw; break;
    }
    return v;
}

bool fitsType(ValueType type, double base) {
    switch (type) {
        case ValueType::Int8:    return fitsInt8(base);
        case ValueType::UInt8:   return fitsUint8(base);
        case ValueType::Int16:   return fitsInt16(base);
        case ValueType::UInt16:  return fitsUint16(base);
        case ValueType::Int32:   return fitsInt32(base);
        case ValueType::UInt32:  return fitsUint32(base);
        case ValueType::Int64:   return fitsInt64(base);
        case ValueType::UInt64:  return fitsUint64(base);
        case ValueType::Float32:
        case ValueType::Float64: return std::isfinite(base);
    }
    return false;
}

void appendNumericVariant(QList<ValueVariant>& out, const QString& label, ValueType type, double base, bool secondary) {
    if (!fitsType(type, base)) return;

    ValueVariant variant;
    variant.value = makeScanValue(type, base);
    variant.label = label;
    variant.secondary = secondary;
    out.append(variant);
}

void appendScaledIntVariants(QList<ValueVariant>& out, ValueType type, double base, bool secondary) {
    const QString typeName = valueTypeToString(type);
    appendNumericVariant(out, typeName, type, base, secondary);

    static const QList<QPair<int, QString>> scales = {
        {10, "x10"},
        {100, "x100"},
        {1000, "x1000"},
        {4096, "x4096"},
        {65536, "x65536"},
    };
    for (const auto& scale : scales) {
        appendNumericVariant(out, QString("%1 %2").arg(typeName, scale.second), type, base * scale.first, true);
    }
}

void appendFloatVariants(QList<ValueVariant>& out, const QString& label, ValueType type, double base, bool secondary) {
    appendNumericVariant(out, label, type, base, secondary);

    // Variantes de scaling fréquentes pour les scores/argent internes.
    static const QList<QPair<int, QString>> scales = {
        {10, "x10"},
        {100, "x100"},
        {1000, "x1000"},
        {4096, "x4096"},
        {65536, "x65536"},
    };
    for (const auto& scale : scales) {
        appendNumericVariant(out, QString("%1 %2").arg(label, scale.second), type, base * scale.first, true);
    }
}

void appendVariantsForType(QList<ValueVariant>& out, ValueType type, double base) {
    const QString typeName = valueTypeToString(type);
    switch (type) {
        case ValueType::Int8:
        case ValueType::UInt8:
        case ValueType::Int16:
        case ValueType::UInt16:
        case ValueType::Int32:
        case ValueType::UInt32:
        case ValueType::Int64:
        case ValueType::UInt64:
            appendScaledIntVariants(out, type, base, /*secondary=*/false);
            break;
        case ValueType::Float32:
        case ValueType::Float64:
            appendFloatVariants(out, typeName, type, base, /*secondary=*/false);
            break;
    }
}

void appendCrossIntVariants(QList<ValueVariant>& out, double base, bool secondary) {
    // Les entiers peuvent aussi être stockés en float/double dans le jeu.
    if (fitsInt32(base)) {
        appendFloatVariants(out, "Float32 from Int32", ValueType::Float32, base, secondary);
        appendFloatVariants(out, "Float64 from Int32", ValueType::Float64, base, secondary);
    }
}

QList<ValueVariant> deduplicatedByBytes(const QList<ValueVariant>& variants) {
    QList<ValueVariant> out;
    QSet<QByteArray> seen;
    out.reserve(variants.size());
    for (const auto& variant : variants) {
        const QByteArray bytes = scanValueToBytes(variant.value);
        if (bytes.isEmpty() || seen.contains(bytes)) {
            continue;
        }
        seen.insert(bytes);
        out.append(variant);
    }
    return out;
}

} // namespace

QList<ValueVariant> generateScanVariants(
    const QString& rawValue,
    ValueType explicitType,
    bool explicitTypeGiven) {
    QList<ValueVariant> out;

    double base = 0.0;
    if (!parseNumber(rawValue, &base)) {
        return out;
    }

    if (explicitTypeGiven) {
        appendVariantsForType(out, explicitType, base);
        return deduplicatedByBytes(out);
    }

    // Multi-type automatique : assez large pour les moteurs de jeu, mais borné
    // pour éviter les types 8-bit et les floats scalés qui matchent partout.
    if (fitsInt16(base)) {
        appendNumericVariant(out, "Int16", ValueType::Int16, base, true);
    }
    if (fitsUint16(base)) {
        appendNumericVariant(out, "UInt16", ValueType::UInt16, base, true);
    }
    if (fitsInt32(base)) {
        appendScaledIntVariants(out, ValueType::Int32, base, false);
    }
    if (fitsUint32(base)) {
        appendScaledIntVariants(out, ValueType::UInt32, base, true);
    }
    if (fitsInt64(base)) {
        appendScaledIntVariants(out, ValueType::Int64, base, true);
    }
    if (fitsUint64(base)) {
        appendScaledIntVariants(out, ValueType::UInt64, base, true);
    }
    appendNumericVariant(out, "Float32", ValueType::Float32, base, false);
    appendNumericVariant(out, "Float64", ValueType::Float64, base, true);

    return deduplicatedByBytes(out);
}

} // namespace killcore
