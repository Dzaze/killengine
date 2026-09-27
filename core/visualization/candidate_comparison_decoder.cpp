#include "candidate_comparison_decoder.h"

#include "scanner/value_variants.h"

#include <QByteArray>

#include <cmath>
#include <cstring>

namespace killcore {

namespace {

bool valueTypeIsIntegerType(ValueType type) {
    switch (type) {
        case ValueType::Float32:
        case ValueType::Float64:
            return false;
        default:
            return true;
    }
}

QByteArray toByteArray(const std::vector<uint8_t>& bytes) {
    return QByteArray(reinterpret_cast<const char*>(bytes.data()), static_cast<qsizetype>(bytes.size()));
}

/// Division exacte d'un texte entier base-10 (signe optionnel) par 10^n,
/// par déplacement de la virgule en texte -- aucune arithmétique flottante.
/// `n` doit correspondre à un facteur décimal usuel (1/10/100/1000).
QString shiftDecimalPoint(const QString& integerText, int n) {
    if (n <= 0 || integerText.isEmpty()) {
        return integerText;
    }
    bool negative = integerText.startsWith(QLatin1Char('-'));
    QString digits = negative ? integerText.mid(1) : integerText;
    while (digits.size() <= n) {
        digits.prepend(QLatin1Char('0'));
    }
    QString wholePart = digits.left(digits.size() - n);
    QString fracPart = digits.right(n);
    // Retire les zéros de fin non significatifs (6000/100 -> "60", pas "60.00").
    while (fracPart.endsWith(QLatin1Char('0'))) {
        fracPart.chop(1);
    }
    QString result = fracPart.isEmpty() ? wholePart : (wholePart + QLatin1Char('.') + fracPart);
    return negative ? (QLatin1Char('-') + result) : result;
}

} // namespace

DecodedComparisonValue decodeComparisonValue(const std::vector<uint8_t>& bytes, ValueType type) {
    DecodedComparisonValue result;
    if (bytes.size() < valueTypeSize(type)) {
        return result;
    }

    const QByteArray asQt = toByteArray(bytes);
    result.exactValueText = scanBytesToExactString(asQt, type);
    if (result.exactValueText.isEmpty()) {
        return result;
    }
    result.numericValue = scanBytesToDouble(asQt, type);
    result.ok = true;

    if (type == ValueType::Float32) {
        float raw = 0.0f;
        std::memcpy(&raw, bytes.data(), sizeof(raw));
        result.isNaN = std::isnan(raw);
        result.isInfinite = std::isinf(raw);
    } else if (type == ValueType::Float64) {
        double raw = 0.0;
        std::memcpy(&raw, bytes.data(), sizeof(raw));
        result.isNaN = std::isnan(raw);
        result.isInfinite = std::isinf(raw);
    }

    return result;
}

QString formatScaledValueText(const DecodedComparisonValue& decoded, ValueType type, double factor, bool* outExact) {
    if (outExact) {
        *outExact = false;
    }
    if (!decoded.ok) {
        return QString();
    }
    if (decoded.isNaN || decoded.isInfinite) {
        // Un facteur ne change rien à NaN/Inf -- jamais de division dessus.
        return decoded.exactValueText;
    }
    if (!(factor > 0.0) || std::isnan(factor) || std::isinf(factor)) {
        // factor <= 0, NaN ou infini : repli documenté sur la valeur brute.
        return decoded.exactValueText;
    }
    if (factor == 1.0) {
        if (outExact) *outExact = true;
        return decoded.exactValueText;
    }

    if (valueTypeIsIntegerType(type)) {
        int n = -1;
        if (factor == 10.0) n = 1;
        else if (factor == 100.0) n = 2;
        else if (factor == 1000.0) n = 3;
        if (n >= 0) {
            if (outExact) *outExact = true;
            return shiftDecimalPoint(decoded.exactValueText, n);
        }
    }

    // Facteur binaire (4096/65536) ou type flottant scalé : approximation
    // double documentée, pas une vraie exactitude pour ces échelles-là.
    return QString::number(decoded.numericValue / factor, 'g', 15);
}

} // namespace killcore
