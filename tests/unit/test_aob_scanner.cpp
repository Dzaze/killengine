#include "patch/aob_scanner.h"

#include <gtest/gtest.h>

using namespace killcore;

TEST(AobScanner, ParsesHexBytesAndWildcards) {
    const auto pattern = parseAobPattern("48 8B ?? 10 0xFF ?");

    ASSERT_TRUE(pattern.isValid()) << pattern.error.toStdString();
    ASSERT_EQ(pattern.bytes.size(), 6u);
    EXPECT_EQ(pattern.bytes[0], 0x48);
    EXPECT_EQ(pattern.bytes[1], 0x8B);
    EXPECT_FALSE(pattern.bytes[2].has_value());
    EXPECT_EQ(pattern.bytes[3], 0x10);
    EXPECT_EQ(pattern.bytes[4], 0xFF);
    EXPECT_FALSE(pattern.bytes[5].has_value());
}

TEST(AobScanner, RejectsInvalidTokens) {
    const auto pattern = parseAobPattern("48 8B XYZ");

    EXPECT_FALSE(pattern.isValid());
    EXPECT_FALSE(pattern.error.isEmpty());
}

TEST(AobScanner, FindsPatternInBuffer) {
    const QByteArray haystack = QByteArray::fromHex("9090488B3412CC488B9910");
    const auto pattern = parseAobPattern("48 8B ?? 12");

    const auto matches = searchAobBuffer(haystack, pattern, 0x1000);

    ASSERT_EQ(matches.size(), 1);
    EXPECT_EQ(matches.first(), 0x1002u);
}

TEST(AobScanner, FindsMultipleWildcardMatches) {
    const QByteArray haystack = QByteArray::fromHex("AA11BBAA22BBAA33CC");
    const auto pattern = parseAobPattern("AA ?? BB");

    const auto matches = searchAobBuffer(haystack, pattern, 0);

    ASSERT_EQ(matches.size(), 2);
    EXPECT_EQ(matches[0], 0u);
    EXPECT_EQ(matches[1], 3u);
}

TEST(AobScanner, FormatsBytesAsAobPattern) {
    const QByteArray bytes = QByteArray::fromHex("488B05DEADBEEF");

    EXPECT_EQ(bytesToAobPattern(bytes), "48 8B 05 DE AD BE EF");
}
