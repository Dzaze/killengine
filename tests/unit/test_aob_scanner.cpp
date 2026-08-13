#include "patch/aob_scanner.h"
#include "patch/code_patch.h"
#include "patch/instruction_patch_suggester.h"

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

TEST(CodePatch, ParsesExactPatchBytes) {
    const auto bytes = parsePatchBytes("90 90 0xCC");

    ASSERT_TRUE(bytes.isValid()) << bytes.error.toStdString();
    EXPECT_EQ(bytes.bytes, QByteArray::fromHex("9090CC"));
}

TEST(CodePatch, RejectsWildcardPatchBytes) {
    const auto bytes = parsePatchBytes("90 ?? CC");

    EXPECT_FALSE(bytes.isValid());
    EXPECT_TRUE(bytes.bytes.isEmpty());
}

TEST(InstructionPatchSuggester, DecodesCommonRipRelativeWriteLength) {
    const QByteArray bytes = QByteArray::fromHex("488905DEADBEEF488B05");

    const auto instruction = decodeX64InstructionLength(bytes);

    ASSERT_TRUE(instruction.success) << instruction.error.toStdString();
    EXPECT_EQ(instruction.length, 7);
    EXPECT_EQ(instruction.mnemonicHint, "write-like");
}

TEST(InstructionPatchSuggester, SuggestsSameLengthNopPatch) {
    InstructionInfo instruction;
    instruction.success = true;
    instruction.length = 7;
    instruction.mnemonicHint = "write-like";

    const auto suggestions = suggestInstructionPatches(instruction);

    ASSERT_FALSE(suggestions.isEmpty());
    EXPECT_EQ(suggestions.first().label, "NOP x7");
    EXPECT_EQ(suggestions.first().bytesText, "90 90 90 90 90 90 90");
    EXPECT_FALSE(suggestions.first().risky);
}
