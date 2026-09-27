#include "visualization/candidate_comparison_decoder.h"

#include <gtest/gtest.h>

#include <cstring>
#include <limits>

using namespace killcore;

namespace {

template <typename T>
std::vector<uint8_t> bytesFrom(T value) {
    std::vector<uint8_t> bytes(sizeof(T));
    std::memcpy(bytes.data(), &value, sizeof(T));
    return bytes;
}

} // namespace

TEST(CandidateComparisonDecoder, DecodesNegativeInt32) {
    const auto decoded = decodeComparisonValue(bytesFrom<int32_t>(-500), ValueType::Int32);
    ASSERT_TRUE(decoded.ok);
    EXPECT_EQ(decoded.exactValueText, "-500");
    EXPECT_DOUBLE_EQ(decoded.numericValue, -500.0);
    EXPECT_FALSE(decoded.isNaN);
    EXPECT_FALSE(decoded.isInfinite);
}

TEST(CandidateComparisonDecoder, DecodesUInt64BeyondDoublePrecisionExactly) {
    const uint64_t value = (uint64_t(1) << 53) + 1;
    const auto decoded = decodeComparisonValue(bytesFrom<uint64_t>(value), ValueType::UInt64);
    ASSERT_TRUE(decoded.ok);
    EXPECT_EQ(decoded.exactValueText, "9007199254740993");
}

TEST(CandidateComparisonDecoder, DeltaOfOneNearInt32BoundaryStaysDistinguishable) {
    const int32_t nearMax = std::numeric_limits<int32_t>::max() - 1;
    const auto a = decodeComparisonValue(bytesFrom<int32_t>(nearMax), ValueType::Int32);
    const auto b = decodeComparisonValue(bytesFrom<int32_t>(nearMax + 1), ValueType::Int32);
    ASSERT_TRUE(a.ok);
    ASSERT_TRUE(b.ok);
    EXPECT_NE(a.exactValueText, b.exactValueText);
    EXPECT_EQ(b.numericValue - a.numericValue, 1.0);
}

TEST(CandidateComparisonDecoder, Float32AndFloat64DecodeCorrectly) {
    const auto f32 = decodeComparisonValue(bytesFrom<float>(3.5f), ValueType::Float32);
    ASSERT_TRUE(f32.ok);
    EXPECT_DOUBLE_EQ(f32.numericValue, 3.5);
    EXPECT_FALSE(f32.isNaN);
    EXPECT_FALSE(f32.isInfinite);

    const auto f64 = decodeComparisonValue(bytesFrom<double>(1.0 / 3.0), ValueType::Float64);
    ASSERT_TRUE(f64.ok);
    EXPECT_DOUBLE_EQ(f64.numericValue, 1.0 / 3.0);
}

TEST(CandidateComparisonDecoder, NaNAndInfiniteAreFlaggedNotSilentlyTreatedAsZero) {
    const auto nan32 = decodeComparisonValue(bytesFrom<float>(std::numeric_limits<float>::quiet_NaN()), ValueType::Float32);
    ASSERT_TRUE(nan32.ok);
    EXPECT_TRUE(nan32.isNaN);

    const auto inf64 = decodeComparisonValue(bytesFrom<double>(std::numeric_limits<double>::infinity()), ValueType::Float64);
    ASSERT_TRUE(inf64.ok);
    EXPECT_TRUE(inf64.isInfinite);
}

TEST(CandidateComparisonDecoder, TruncatedBytesAreNotOkNeverFabricateAValue) {
    std::vector<uint8_t> tooShort(2, 0);
    const auto decoded = decodeComparisonValue(tooShort, ValueType::Int32);
    EXPECT_FALSE(decoded.ok);
    EXPECT_TRUE(decoded.exactValueText.isEmpty());
}

TEST(CandidateComparisonDecoder, EmptyBytesAreNotOk) {
    const auto decoded = decodeComparisonValue({}, ValueType::Int64);
    EXPECT_FALSE(decoded.ok);
}

// ---------------------------------------------------------------------------
// formatScaledValueText
// ---------------------------------------------------------------------------

TEST(FormatScaledValueText, FactorOneReturnsExactTextUnchanged) {
    const auto decoded = decodeComparisonValue(bytesFrom<int32_t>(12345), ValueType::Int32);
    bool exact = false;
    const QString text = formatScaledValueText(decoded, ValueType::Int32, 1.0, &exact);
    EXPECT_TRUE(exact);
    EXPECT_EQ(text, "12345");
}

TEST(FormatScaledValueText, FactorOneHundredDividesExactlyViaText) {
    const auto decoded = decodeComparisonValue(bytesFrom<int32_t>(6000), ValueType::Int32);
    bool exact = false;
    const QString text = formatScaledValueText(decoded, ValueType::Int32, 100.0, &exact);
    EXPECT_TRUE(exact);
    EXPECT_EQ(text, "60");
}

TEST(FormatScaledValueText, FactorOneThousandKeepsFractionalDigits) {
    const auto decoded = decodeComparisonValue(bytesFrom<int32_t>(1234), ValueType::Int32);
    bool exact = false;
    const QString text = formatScaledValueText(decoded, ValueType::Int32, 1000.0, &exact);
    EXPECT_TRUE(exact);
    EXPECT_EQ(text, "1.234");
}

TEST(FormatScaledValueText, NegativeValueScaledExactlyKeepsSign) {
    const auto decoded = decodeComparisonValue(bytesFrom<int32_t>(-250), ValueType::Int32);
    bool exact = false;
    const QString text = formatScaledValueText(decoded, ValueType::Int32, 100.0, &exact);
    EXPECT_TRUE(exact);
    EXPECT_EQ(text, "-2.5");
}

TEST(FormatScaledValueText, BinaryFactor4096FallsBackToApproximationDocumented) {
    const auto decoded = decodeComparisonValue(bytesFrom<int32_t>(4096 * 7), ValueType::Int32);
    bool exact = true;
    const QString text = formatScaledValueText(decoded, ValueType::Int32, 4096.0, &exact);
    EXPECT_FALSE(exact);
    EXPECT_DOUBLE_EQ(text.toDouble(), 7.0);
}

TEST(FormatScaledValueText, BinaryFactor65536FallsBackToApproximationDocumented) {
    const auto decoded = decodeComparisonValue(bytesFrom<int32_t>(65536 * 3), ValueType::Int32);
    bool exact = true;
    const QString text = formatScaledValueText(decoded, ValueType::Int32, 65536.0, &exact);
    EXPECT_FALSE(exact);
    EXPECT_DOUBLE_EQ(text.toDouble(), 3.0);
}

TEST(FormatScaledValueText, NonPositiveFactorFallsBackToRawExactText) {
    const auto decoded = decodeComparisonValue(bytesFrom<int32_t>(42), ValueType::Int32);
    bool exact = true;
    const QString text = formatScaledValueText(decoded, ValueType::Int32, 0.0, &exact);
    EXPECT_FALSE(exact);
    EXPECT_EQ(text, "42");

    bool exact2 = true;
    const QString text2 = formatScaledValueText(decoded, ValueType::Int32, -10.0, &exact2);
    EXPECT_FALSE(exact2);
    EXPECT_EQ(text2, "42");
}

TEST(FormatScaledValueText, NaNFactorFallsBackToRawExactText) {
    const auto decoded = decodeComparisonValue(bytesFrom<int32_t>(42), ValueType::Int32);
    bool exact = true;
    const QString text = formatScaledValueText(decoded, ValueType::Int32, std::numeric_limits<double>::quiet_NaN(), &exact);
    EXPECT_FALSE(exact);
    EXPECT_EQ(text, "42");
}

TEST(FormatScaledValueText, NaNValueIsNeverDividedByFactor) {
    const auto decoded = decodeComparisonValue(bytesFrom<float>(std::numeric_limits<float>::quiet_NaN()), ValueType::Float32);
    ASSERT_TRUE(decoded.ok);
    bool exact = true;
    const QString text = formatScaledValueText(decoded, ValueType::Float32, 100.0, &exact);
    EXPECT_FALSE(exact);
    EXPECT_EQ(text, "NaN");
}

TEST(FormatScaledValueText, NotOkDecodedValueReturnsEmptyText) {
    DecodedComparisonValue decoded; // ok=false par défaut
    bool exact = true;
    const QString text = formatScaledValueText(decoded, ValueType::Int32, 100.0, &exact);
    EXPECT_FALSE(exact);
    EXPECT_TRUE(text.isEmpty());
}

TEST(FormatScaledValueText, FloatTypeScaledFactorAlwaysApproximate) {
    const auto decoded = decodeComparisonValue(bytesFrom<float>(2.5f), ValueType::Float32);
    bool exact = true;
    const QString text = formatScaledValueText(decoded, ValueType::Float32, 100.0, &exact);
    EXPECT_FALSE(exact);
    EXPECT_DOUBLE_EQ(text.toDouble(), 0.025);
}
