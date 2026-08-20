#include "patch/aob_scanner.h"
#include "patch/code_patch.h"
#include "patch/instruction_patch_suggester.h"
#include "patch/profile_patch_state.h"

#include <algorithm>

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

TEST(AobScanner, FindsOverlappingAnchoredMatches) {
    const QByteArray haystack = QByteArray::fromHex("ABABABAB");
    const auto pattern = parseAobPattern("AB AB");

    const auto matches = searchAobBuffer(haystack, pattern, 0x2000);

    ASSERT_EQ(matches.size(), 3);
    EXPECT_EQ(matches[0], 0x2000u);
    EXPECT_EQ(matches[1], 0x2001u);
    EXPECT_EQ(matches[2], 0x2002u);
}

TEST(AobScanner, FindsPatternWithWildcardSuffix) {
    const QByteArray haystack = QByteArray::fromHex("10488B0190488BFF");
    const auto pattern = parseAobPattern("48 8B ??");

    const auto matches = searchAobBuffer(haystack, pattern, 0x3000);

    ASSERT_EQ(matches.size(), 2);
    EXPECT_EQ(matches[0], 0x3001u);
    EXPECT_EQ(matches[1], 0x3005u);
}

TEST(AobScanner, AllWildcardPatternMatchesEveryWindow) {
    const QByteArray haystack = QByteArray::fromHex("01020304");
    const auto pattern = parseAobPattern("?? ??");

    const auto matches = searchAobBuffer(haystack, pattern, 0x4000);

    ASSERT_EQ(matches.size(), 3);
    EXPECT_EQ(matches[0], 0x4000u);
    EXPECT_EQ(matches[1], 0x4001u);
    EXPECT_EQ(matches[2], 0x4002u);
}

TEST(AobScanner, FormatsBytesAsAobPattern) {
    const QByteArray bytes = QByteArray::fromHex("488B05DEADBEEF");

    EXPECT_EQ(bytesToAobPattern(bytes), "48 8B 05 DE AD BE EF");
}

TEST(AobScanner, ScoresStrongPatternAsTrainerSafe) {
    const auto pattern = parseAobPattern("48 89 05 AA BB CC DD 48 8B 0D 11 22 33 44");

    const auto quality = evaluateAobPatternQuality(pattern);

    EXPECT_EQ(quality.level, QString("strong"));
    EXPECT_GE(quality.score, 75);
    EXPECT_TRUE(quality.trainerSafe);
    EXPECT_EQ(quality.fixedBytes, 14);
    EXPECT_EQ(quality.wildcardBytes, 0);
}

TEST(AobScanner, ScoresWildcardHeavyPatternAsWeak) {
    const auto pattern = parseAobPattern("48 ?? ?? ?? ?? ?? ??");

    const auto quality = evaluateAobPatternQuality(pattern);

    EXPECT_EQ(quality.level, QString("weak"));
    EXPECT_LT(quality.score, 50);
    EXPECT_FALSE(quality.trainerSafe);
    EXPECT_EQ(quality.fixedBytes, 1);
    EXPECT_EQ(quality.wildcardBytes, 6);
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

TEST(InstructionPatchSuggester, DecodesImmediateOperandOffsetForMemoryWrite) {
    // mov dword ptr [rax+8], 0x64  ->  C7 40 08 64 00 00 00
    // C7 = opcode (MOV r/m32, imm32) ; 40 = ModRM (mod=01 disp8, reg=/0, rm=rax)
    // 08 = disp8 ; 64 00 00 00 = imm32 (little-endian 0x64)
    const QByteArray bytes = QByteArray::fromHex("C7400864000000");

    const auto instruction = decodeX64InstructionLength(bytes);

    ASSERT_TRUE(instruction.success) << instruction.error.toStdString();
    EXPECT_EQ(instruction.length, 7);
    EXPECT_EQ(instruction.category, "memory-write");
    if (instruction.decoder == "zydis") {
        // L'offset/taille de l'immediat ne sont calcules que via le decodeur
        // Zydis (cf. instruction_patch_suggester.cpp) ; le decodeur builtin
        // de secours ne les remplit pas.
        EXPECT_EQ(instruction.immediateOffset, 3);
        EXPECT_EQ(instruction.immediateSize, 4);
        EXPECT_EQ(instruction.memBaseRegister, QStringLiteral("rax"));
        EXPECT_EQ(instruction.memDisplacement, 8);
    }
}

TEST(InstructionPatchSuggester, DecodesMemoryOperandForRegisterSourceWrite) {
    // mov dword ptr [rdi+8], eax  ->  89 47 08
    // 89 = opcode (MOV r/m32, r32) ; 47 = ModRM (mod=01 disp8, reg=eax, rm=rdi)
    // 08 = disp8 ; pas d'immediat (source = registre eax)
    const QByteArray bytes = QByteArray::fromHex("894708");

    const auto instruction = decodeX64InstructionLength(bytes);

    ASSERT_TRUE(instruction.success) << instruction.error.toStdString();
    EXPECT_EQ(instruction.length, 3);
    EXPECT_EQ(instruction.category, "memory-write");
    if (instruction.decoder == "zydis") {
        // Pas d'immediat a substituer (source registre), mais l'operande
        // memoire destination doit rester exploitable pour construire un
        // trampoline "force cette valeur ici" (cf. ApplicationController::
        // forceWriteInstructionValue).
        EXPECT_EQ(instruction.immediateSize, 0);
        EXPECT_EQ(instruction.memBaseRegister, QStringLiteral("rdi"));
        EXPECT_EQ(instruction.memDisplacement, 8);
    }
}

TEST(InstructionPatchSuggester, LeavesMemBaseRegisterEmptyForRipRelativeWrite) {
    // Reprend DecodesCommonRipRelativeWriteLength ("mov [rip+...], rax") :
    // RIP-relatif est hors de la portee d'encodeMemImmMov, memBaseRegister
    // doit rester vide plutot que de proposer un trampoline qui ecrirait au
    // mauvais endroit.
    const QByteArray bytes = QByteArray::fromHex("488905DEADBEEF488B05");

    const auto instruction = decodeX64InstructionLength(bytes);

    ASSERT_TRUE(instruction.success) << instruction.error.toStdString();
    if (instruction.decoder == "zydis") {
        EXPECT_TRUE(instruction.memBaseRegister.isEmpty());
    }
}

TEST(InstructionPatchSuggester, SuggestsValueOverrideForImmediateMemoryWrite) {
    InstructionInfo instruction;
    instruction.success = true;
    instruction.length = 7;
    instruction.mnemonicHint = "write-like";
    instruction.category = "memory-write";
    instruction.rawBytesText = "C7 40 08 64 00 00 00";
    instruction.immediateOffset = 3;
    instruction.immediateSize = 4;

    const auto suggestions = suggestInstructionPatches(instruction);

    const auto valueOverride = std::find_if(suggestions.begin(), suggestions.end(), [](const auto& s) {
        return s.needsValueInput;
    });
    ASSERT_NE(valueOverride, suggestions.end());
    EXPECT_EQ(valueOverride->label, "Forcer une valeur");
    EXPECT_EQ(valueOverride->valueOffset, 3);
    EXPECT_EQ(valueOverride->valueSize, 4);
    // Le NOP reste la premiere suggestion (non risquee, choisie par defaut) :
    // "Forcer une valeur" s'ajoute sans changer le comportement existant.
    EXPECT_FALSE(suggestions.first().needsValueInput);
}

TEST(InstructionPatchSuggester, DoesNotSuggestValueOverrideWithoutImmediate) {
    InstructionInfo instruction;
    instruction.success = true;
    instruction.length = 7;
    instruction.mnemonicHint = "write-like";
    instruction.category = "memory-write";
    instruction.rawBytesText = "48 89 05 DE AD BE EF";
    // Pas d'immediat (source = registre, ex: mov [rip+disp], rax) : immediateSize
    // reste a 0 par defaut, aucune suggestion "Forcer une valeur" ne doit sortir.

    const auto suggestions = suggestInstructionPatches(instruction);

    const auto valueOverride = std::find_if(suggestions.begin(), suggestions.end(), [](const auto& s) {
        return s.needsValueInput;
    });
    EXPECT_EQ(valueOverride, suggestions.end());
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
