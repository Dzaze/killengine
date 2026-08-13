#include "patch/aob_scanner.h"
#include "patch/code_patch.h"
#include "patch/instruction_patch_suggester.h"
#include "patch/profile_patch_state.h"

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
    EXPECT_FALSE(instruction.mnemonicHint.isEmpty());
    EXPECT_FALSE(instruction.decoder.isEmpty());
    EXPECT_EQ(instruction.category, "memory-write");
    EXPECT_FALSE(instruction.rawBytesText.isEmpty());
    EXPECT_EQ(instruction.stableAobPattern, "48 89 05 ?? ?? ?? ??");
}

TEST(InstructionPatchSuggester, SuggestsSameLengthNopPatch) {
    InstructionInfo instruction;
    instruction.success = true;
    instruction.length = 7;
    instruction.mnemonicHint = "write-like";
    instruction.category = "memory-write";

    const auto suggestions = suggestInstructionPatches(instruction);

    ASSERT_FALSE(suggestions.isEmpty());
    EXPECT_EQ(suggestions.first().label, "NOP écriture x7");
    EXPECT_EQ(suggestions.first().bytesText, "90 90 90 90 90 90 90");
    EXPECT_EQ(suggestions.first().riskLevel, "low");
    EXPECT_FALSE(suggestions.first().risky);
}

TEST(InstructionPatchSuggester, SuggestsBranchDirectionPatches) {
    const QByteArray bytes = QByteArray::fromHex("7505");
    auto instruction = decodeX64InstructionLength(bytes);
    ASSERT_TRUE(instruction.success) << instruction.error.toStdString();
    ASSERT_EQ(instruction.category, "conditional-jump");

    const auto suggestions = suggestInstructionPatches(instruction);

    ASSERT_GE(suggestions.size(), 2);
    EXPECT_EQ(suggestions[0].label, "Forcer non pris");
    EXPECT_EQ(suggestions[0].bytesText, "90 90");
    EXPECT_EQ(suggestions[1].label, "Forcer pris");
    EXPECT_EQ(suggestions[1].bytesText, "EB 05");
}

TEST(ProfilePatchState, ClassifiesOriginalCode) {
    const auto state = classifyProfilePatchMemoryState(1, 0, false, true);

    EXPECT_EQ(state.status, "original");
    EXPECT_TRUE(state.success);
    EXPECT_FALSE(state.active);
}

TEST(ProfilePatchState, ClassifiesActivePatchedCode) {
    const auto state = classifyProfilePatchMemoryState(0, 1, false, true);

    EXPECT_EQ(state.status, "active");
    EXPECT_TRUE(state.success);
    EXPECT_TRUE(state.active);
}

TEST(ProfilePatchState, KeepsSessionActiveAsActive) {
    const auto state = classifyProfilePatchMemoryState(1, 1, true, true);

    EXPECT_EQ(state.status, "active");
    EXPECT_TRUE(state.success);
    EXPECT_TRUE(state.active);
}

TEST(ProfilePatchState, ClassifiesMissingSignaturesAsFailure) {
    const auto state = classifyProfilePatchMemoryState(0, 0, false, true);

    EXPECT_EQ(state.status, "missing");
    EXPECT_FALSE(state.success);
    EXPECT_FALSE(state.active);
}

TEST(ProfilePatchState, ClassifiesMultipleMatchesAsAmbiguous) {
    const auto state = classifyProfilePatchMemoryState(2, 0, false, true);

    EXPECT_EQ(state.status, "ambiguous");
    EXPECT_TRUE(state.success);
    EXPECT_FALSE(state.active);
}

TEST(ProfilePatchState, ClassifiesInvalidPatternsAsFailure) {
    const auto state = classifyProfilePatchMemoryState(0, 0, false, false);

    EXPECT_EQ(state.status, "invalid");
    EXPECT_FALSE(state.success);
    EXPECT_FALSE(state.active);
}
