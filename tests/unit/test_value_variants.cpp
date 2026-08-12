#include "scanner/value_variants.h"

#include <gtest/gtest.h>

#include <QByteArray>
#include <cstring>

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
