#include "scanner/value_variants.h"

#include <gtest/gtest.h>

#include <QByteArray>
#include <cstring>
#include <limits>

using killcore::generateScanVariants;
using killcore::scanValueToBytes;
using killcore::ValueType;

namespace {

bool variantHasValue(const QList<killcore::ValueVariant>& variants, ValueType type, double expected) {
    for (const auto& v : variants) {
        if (v.value.type != type) continue;
        killcore::ScanValue expectedSv;
        expectedSv.type = type;
        switch (type) {
            case ValueType::Int8:    expectedSv.value = static_cast<int8_t>(expected); break;
            case ValueType::UInt8:   expectedSv.value = static_cast<uint>(expected); break;
            case ValueType::Int16:   expectedSv.value = static_cast<int16_t>(expected); break;
            case ValueType::UInt16:  expectedSv.value = static_cast<uint>(expected); break;
            case ValueType::Int32:   expectedSv.value = static_cast<int32_t>(expected); break;
            case ValueType::UInt32:  expectedSv.value = static_cast<uint>(expected); break;
            case ValueType::Int64:   expectedSv.value = static_cast<int64_t>(expected); break;
            case ValueType::UInt64:  expectedSv.value = static_cast<qulonglong>(expected); break;
            case ValueType::Float32: expectedSv.value = static_cast<float>(expected); break;
            case ValueType::Float64: expectedSv.value = expected; break;
        }
        const QByteArray a = scanValueToBytes(v.value);
        const QByteArray b = scanValueToBytes(expectedSv);
        if (a.size() == b.size() && std::memcmp(a.constData(), b.constData(), a.size()) == 0) {
            return true;
        }
    }
    return false;
}

}

TEST(ValueVariants, EmptyForNonNumeric) {
    const auto variants = generateScanVariants("not-a-number");
    EXPECT_TRUE(variants.isEmpty());
}

TEST(ValueVariants, MultiTypeWhenNotExplicit) {
    const auto variants = generateScanVariants("100");
    EXPECT_TRUE(variantHasValue(variants, ValueType::Int32, 100.0));
    EXPECT_TRUE(variantHasValue(variants, ValueType::Int16, 100.0));
    EXPECT_TRUE(variantHasValue(variants, ValueType::Float32, 100.0));
    EXPECT_TRUE(variantHasValue(variants, ValueType::Float64, 100.0));
}

TEST(ValueVariants, ExplicitTypeOnly) {
    const auto variants = generateScanVariants("50", ValueType::Int32, true);
    EXPECT_TRUE(variantHasValue(variants, ValueType::Int32, 50.0));
    EXPECT_FALSE(variantHasValue(variants, ValueType::Float64, 50.0));
}

TEST(ValueVariants, ScalingVariantsForFloat) {
    const auto variants = generateScanVariants("1.5", ValueType::Float32, true);
    EXPECT_TRUE(variantHasValue(variants, ValueType::Float32, 1.5));
    EXPECT_TRUE(variantHasValue(variants, ValueType::Float32, 15.0));
    EXPECT_TRUE(variantHasValue(variants, ValueType::Float32, 150.0));
    EXPECT_TRUE(variantHasValue(variants, ValueType::Float32, 1500.0));
}

TEST(ValueVariants, SecondaryFlagForScaledVariants) {
    const auto variants = generateScanVariants("100", ValueType::Float32, true);
    bool hasSecondary = false;
    bool hasPrimary = false;
    for (const auto& v : variants) {
        if (v.secondary) hasSecondary = true;
        else hasPrimary = true;
    }
    EXPECT_TRUE(hasPrimary);
    EXPECT_TRUE(hasSecondary);
}

TEST(ValueVariants, LargeValueSkipsInt32) {
    const auto variants = generateScanVariants("5000000000");
    EXPECT_FALSE(variantHasValue(variants, ValueType::Int32, 5000000000.0));
    EXPECT_TRUE(variantHasValue(variants, ValueType::Int64, 5000000000.0));
}

TEST(ValueVariants, IncludesCompactUnsignedAndFixedPointScales) {
    const auto variants = generateScanVariants("35");
    EXPECT_LE(variants.size(), 20);
    EXPECT_TRUE(variantHasValue(variants, ValueType::Int16, 35.0));
    EXPECT_TRUE(variantHasValue(variants, ValueType::Int32, 35.0 * 4096.0));
    EXPECT_TRUE(variantHasValue(variants, ValueType::Int32, 35.0 * 65536.0));
    EXPECT_FALSE(variantHasValue(variants, ValueType::UInt8, 35.0));
}

// SC2-UNKNOWN-1 : le scan Unknown en mode Delta doit pouvoir traduire un
// delta affiché (ex. +7) en delta brut scalé (ex. +28672 pour x4096), le
// même trou que generateScanVariants comblait déjà pour le scan exact.

bool deltaVariantHasValue(const QList<killcore::DeltaVariant>& variants, double expectedRawDelta, const QString& labelSuffix) {
    for (const auto& v : variants) {
        if (std::abs(v.rawDelta - expectedRawDelta) < 0.0001 && v.label.endsWith(labelSuffix)) {
            return true;
        }
    }
    return false;
}

TEST(ValueVariants, DeltaVariantsIncludeUnscaledAndFixedPointScales) {
    const auto variants = killcore::generateDeltaVariants(7.0, ValueType::Int32);
    EXPECT_TRUE(deltaVariantHasValue(variants, 7.0, "Int32"));
    EXPECT_TRUE(deltaVariantHasValue(variants, 70.0, "x10"));
    EXPECT_TRUE(deltaVariantHasValue(variants, 700.0, "x100"));
    EXPECT_TRUE(deltaVariantHasValue(variants, 7000.0, "x1000"));
    EXPECT_TRUE(deltaVariantHasValue(variants, 28672.0, "x4096"));
    EXPECT_TRUE(deltaVariantHasValue(variants, 458752.0, "x65536"));
    EXPECT_EQ(variants.size(), 6);
}

TEST(ValueVariants, DeltaVariantsPreserveSign) {
    const auto variants = killcore::generateDeltaVariants(-3.0, ValueType::Int16);
    EXPECT_TRUE(deltaVariantHasValue(variants, -3.0, "Int16"));
    EXPECT_TRUE(deltaVariantHasValue(variants, -12288.0, "x4096"));
}

TEST(ValueVariants, DeltaVariantsSkipScalingForFloatTypes) {
    const auto variantsF32 = killcore::generateDeltaVariants(2.5, ValueType::Float32);
    ASSERT_EQ(variantsF32.size(), 1);
    EXPECT_DOUBLE_EQ(variantsF32.first().rawDelta, 2.5);
    EXPECT_EQ(variantsF32.first().label, "Float32");

    const auto variantsF64 = killcore::generateDeltaVariants(2.5, ValueType::Float64);
    ASSERT_EQ(variantsF64.size(), 1);
    EXPECT_EQ(variantsF64.first().label, "Float64");
}

namespace {

template <typename T>
QByteArray bytesFrom(T value) {
    QByteArray bytes(sizeof(T), '\0');
    std::memcpy(bytes.data(), &value, sizeof(T));
    return bytes;
}

} // namespace

using killcore::scanBytesToExactString;

TEST(ValueVariants, ExactStringNegativeInt32) {
    EXPECT_EQ(scanBytesToExactString(bytesFrom<int32_t>(-12345), ValueType::Int32), "-12345");
}

TEST(ValueVariants, ExactStringUInt64BeyondDoublePrecision) {
    // 2^53 + 1 -- pas representable exactement en double, doit rester exact en texte.
    const uint64_t value = (uint64_t(1) << 53) + 1;
    EXPECT_EQ(scanBytesToExactString(bytesFrom<uint64_t>(value), ValueType::UInt64), "9007199254740993");
}

TEST(ValueVariants, ExactStringInt64Max) {
    EXPECT_EQ(scanBytesToExactString(bytesFrom<int64_t>(std::numeric_limits<int64_t>::max()), ValueType::Int64),
              "9223372036854775807");
}

TEST(ValueVariants, ExactStringInt64Min) {
    EXPECT_EQ(scanBytesToExactString(bytesFrom<int64_t>(std::numeric_limits<int64_t>::min()), ValueType::Int64),
              "-9223372036854775808");
}

TEST(ValueVariants, ExactStringDeltaOneNearInt32Boundary) {
    const int32_t nearMax = std::numeric_limits<int32_t>::max() - 1;
    EXPECT_EQ(scanBytesToExactString(bytesFrom<int32_t>(nearMax), ValueType::Int32), "2147483646");
    EXPECT_EQ(scanBytesToExactString(bytesFrom<int32_t>(nearMax + 1), ValueType::Int32), "2147483647");
}

TEST(ValueVariants, ExactStringFloat32RoundTrips) {
    const QString text = scanBytesToExactString(bytesFrom<float>(3.5f), ValueType::Float32);
    EXPECT_DOUBLE_EQ(text.toDouble(), 3.5);
}

TEST(ValueVariants, ExactStringFloat64RoundTrips) {
    const QString text = scanBytesToExactString(bytesFrom<double>(1.0 / 3.0), ValueType::Float64);
    EXPECT_DOUBLE_EQ(text.toDouble(), 1.0 / 3.0);
}

TEST(ValueVariants, ExactStringFloat32NaNAndInfinite) {
    EXPECT_EQ(scanBytesToExactString(bytesFrom<float>(std::numeric_limits<float>::quiet_NaN()), ValueType::Float32), "NaN");
    EXPECT_EQ(scanBytesToExactString(bytesFrom<float>(std::numeric_limits<float>::infinity()), ValueType::Float32), "Inf");
    EXPECT_EQ(scanBytesToExactString(bytesFrom<float>(-std::numeric_limits<float>::infinity()), ValueType::Float32), "-Inf");
}

TEST(ValueVariants, ExactStringFloat64NaNAndInfinite) {
    EXPECT_EQ(scanBytesToExactString(bytesFrom<double>(std::numeric_limits<double>::quiet_NaN()), ValueType::Float64), "NaN");
    EXPECT_EQ(scanBytesToExactString(bytesFrom<double>(std::numeric_limits<double>::infinity()), ValueType::Float64), "Inf");
}

TEST(ValueVariants, ExactStringEmptyForTruncatedBytes) {
    QByteArray tooShort(2, '\0');
    EXPECT_EQ(scanBytesToExactString(tooShort, ValueType::Int32), "");
    EXPECT_EQ(scanBytesToExactString(QByteArray(), ValueType::Int64), "");
}
