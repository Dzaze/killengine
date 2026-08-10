#include "value_variants.h"

#include <limits>
#include <cmath>

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

bool fitsInt64(double v) {
    return v >= static_cast<double>(std::numeric_limits<int64_t>::min())
        && v <= static_cast<double>(std::numeric_limits<int64_t>::max())
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
        case ValueType::Int32:   v.value = static_cast<int32_t>(raw); break;
        case ValueType::Int64:   v.value = static_cast<int64_t>(raw); break;
        case ValueType::Float32: v.value = static_cast<float>(raw); break;
        case ValueType::Float64: v.value = raw; break;
    }
    return v;
}

void appendIntVariants(QList<ValueVariant>& out, const QString& label, ValueType type, double base, bool secondary) {
    if (!fitsInt32(base) && type == ValueType::Int32) return;
    if (!fitsInt64(base) && type == ValueType::Int64) return;

    ValueVariant variant;
    variant.value = makeScanValue(type, base);
    variant.label = label;
    variant.secondary = secondary;
    out.append(variant);

    // Représentation unsigned (même encodage binaire pour la magnitude positive).
    if (fitsUint32(base) || fitsUint64(base)) {
        ValueVariant unsignedVariant;
        unsignedVariant.value = makeScanValue(type, base);
        unsignedVariant.label = label + " unsigned";
        unsignedVariant.secondary = true;
        out.append(unsignedVariant);
    }
}

void appendFloatVariants(QList<ValueVariant>& out, const QString& label, ValueType type, double base, bool secondary) {
    ValueVariant variant;
    variant.value = makeScanValue(type, base);
    variant.label = label;
    variant.secondary = secondary;
    out.append(variant);

    // Variantes de scaling fréquentes pour les scores/argent internes.
    static const QList<QPair<int, QString>> scales = {
        {10, "x10"},
        {100, "x100"},
        {1000, "x1000"},
    };
    for (const auto& scale : scales) {
        const double scaled = base * scale.first;
        ValueVariant scaledVariant;
        scaledVariant.value = makeScanValue(type, scaled);
        scaledVariant.label = QString("%1 %2").arg(label, scale.second);
        scaledVariant.secondary = true;
        out.append(scaledVariant);
    }
}

void appendVariantsForType(QList<ValueVariant>& out, ValueType type, double base) {
    const QString typeName = valueTypeToString(type);
    switch (type) {
        case ValueType::Int32:
        case ValueType::Int64:
            appendIntVariants(out, typeName, type, base, /*secondary=*/false);
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
        return out;
    }

    // Multi-type automatique : Int32 d'abord (le plus courant), puis les autres types.
    if (fitsInt32(base)) {
        appendVariantsForType(out, ValueType::Int32, base);
    }
    if (fitsInt64(base)) {
        appendVariantsForType(out, ValueType::Int64, base);
    }
    appendVariantsForType(out, ValueType::Float32, base);
    appendVariantsForType(out, ValueType::Float64, base);

    // Variantes croisées secondaires (int stocké en float).
    appendCrossIntVariants(out, base, /*secondary=*/true);

    return out;
}

} // namespace killcore