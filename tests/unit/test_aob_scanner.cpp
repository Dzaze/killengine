#include "patch/aob_scanner.h"
#include "patch/code_patch.h"
#include "patch/instruction_patch_suggester.h"
#include "patch/profile_patch_state.h"

#include <algorithm>

#include <QSettings>
#include <gtest/gtest.h>

using namespace killcore;

namespace {

// Sauvegarde/restaure "ui/language" autour du test, meme pattern que
// test_localization.cpp/test_ai_tools.cpp/test_auto_resolver.cpp : les labels
// de suggestion sont desormais traduits via KE_TXT
// (docs/BACKEND_UI_LOCALIZATION_ROADMAP.md, candidat B6, 11/09/2026).
class ScopedUiLanguage {
public:
    explicit ScopedUiLanguage(const QString& language) {
        QSettings settings;
        m_previous = settings.value("ui/language");
        settings.setValue("ui/language", language);
        settings.sync();
    }

    ~ScopedUiLanguage() {
        QSettings settings;
        if (m_previous.isValid()) {
            settings.setValue("ui/language", m_previous);
        } else {
            settings.remove("ui/language");
        }
        settings.sync();
    }

private:
    QVariant m_previous;
};

} // namespace

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
    ScopedUiLanguage lang("fr");
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
    ScopedUiLanguage lang("fr");
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
    ScopedUiLanguage lang("fr");
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

TEST(InstructionPatchSuggester, DisassembleBackwardWindowReconstructsAnimatedCounterSequence) {
    // Reconstitution byte-exacte de la sequence "compteur anime" identifiee
    // pendant l'investigation XP Solitaire (docs/STRATEGY_ROOM.md, entree du
    // 2026-08-20) : actuel/cible entiers a [rsi+0x904]/[rsi+0x908],
    // interpolation flottante, ecriture finale du champ AFFICHE (hors de
    // cette fenetre). La cible connue (targetOffsetInWindow) est la derniere
    // instruction de la sequence, addss xmm0,xmm2.
    const QByteArray bytes = QByteArray::fromHex(
        "8B8E04090000"       // mov      ecx,  [rsi+0x904]   (actuel)
        "F30F108614090000"   // movss    xmm0, [rsi+0x914]
        "F30F5C8618090000"   // subss    xmm0, [rsi+0x918]
        "F30F5E860C090000"   // divss    xmm0, [rsi+0x90C]
        "8B8608090000"       // mov      eax,  [rsi+0x908]   (cible)
        "2BC1"               // sub      eax,  ecx
        "0F57C9"             // xorps    xmm1, xmm1
        "F3480F2AC8"         // cvtsi2ss xmm1, rax
        "F30F59C1"           // mulss    xmm0, xmm1
        "0F57D2"             // xorps    xmm2, xmm2
        "F3480F2AD1"         // cvtsi2ss xmm2, rcx
        "F30F58C2");         // addss    xmm0, xmm2           <- cible (RIP connu)
    const int targetOffset = bytes.size() - 4; // longueur de addss xmm0,xmm2

    const auto result = disassembleBackwardWindow(bytes, targetOffset);

    ASSERT_TRUE(result.success) << result.error.toStdString();
    ASSERT_EQ(result.instructions.size(), 12);
    EXPECT_EQ(result.startOffsetInWindow, 0);

    if (result.instructions.first().decoder == "zydis") {
        EXPECT_EQ(result.instructions[0].mnemonicHint.toLower(), QStringLiteral("mov"));
        EXPECT_EQ(result.instructions[0].memBaseRegister, QStringLiteral("rsi"));
        EXPECT_EQ(result.instructions[0].memDisplacement, 0x904);

        EXPECT_EQ(result.instructions[4].memBaseRegister, QStringLiteral("rsi"));
        EXPECT_EQ(result.instructions[4].memDisplacement, 0x908);

        EXPECT_EQ(result.instructions.last().mnemonicHint.toLower(), QStringLiteral("addss"));
    }
}

TEST(InstructionPatchSuggester, DisassembleBackwardWindowFailsOnMisalignedTarget) {
    // xorps xmm1, xmm1 (3 octets) ; cibler l'offset 1 tombe au milieu de
    // l'instruction, aucun realignement possible depuis un start plus tot
    // (il n'y a pas d'octets avant offset 0) : doit echouer proprement plutot
    // que renvoyer une reconstruction trompeuse.
    const QByteArray bytes = QByteArray::fromHex("0F57C9");

    const auto result = disassembleBackwardWindow(bytes, 1);

    EXPECT_FALSE(result.success);
    EXPECT_FALSE(result.error.isEmpty());
}

TEST(InstructionPatchSuggester, ResolveCandidateFieldAddressesFindsActualAndTargetFields) {
    // Variante du cas Solitaire (docs/STRATEGY_ROOM.md) dont la DERNIERE
    // instruction est l'ecriture memoire elle-meme (mov [rsi+0x900], edx),
    // pas la fin de la chaine de calcul flottant comme dans le test
    // ci-dessus — c'est ce que resolveCandidateFieldAddresses() attend
    // (invariant de disassembleBackwardWindow : la derniere entree est
    // toujours l'instruction qui ecrit sur l'adresse cible connue).
    const QByteArray bytes = QByteArray::fromHex(
        "8B8E04090000"       // mov ecx, [rsi+0x904]   (actuel)
        "8B8608090000"       // mov eax, [rsi+0x908]   (cible)
        "899600090000");     // mov [rsi+0x900], edx   (ecriture, RIP connu)
    const int targetOffset = bytes.size() - 6; // longueur de mov [rsi+0x900], edx

    const auto result = disassembleBackwardWindow(bytes, targetOffset);
    ASSERT_TRUE(result.success) << result.error.toStdString();
    ASSERT_EQ(result.instructions.size(), 3);

    if (result.instructions.last().decoder != "zydis") {
        GTEST_SKIP() << "memBaseRegister non renseigne sans decodeur Zydis (KILLENGINE_HAS_ZYDIS absent).";
    }

    constexpr uint64_t knownWriteTargetAddress = 0x0000700012340900ULL;
    const auto resolved = resolveCandidateFieldAddresses(result.instructions, knownWriteTargetAddress);

    ASSERT_EQ(resolved.size(), 2);

    const auto findByDisplacement = [&resolved](int64_t displacement) -> const ResolvedCandidateField* {
        for (const auto& field : resolved) {
            if (field.memDisplacement == displacement) return &field;
        }
        return nullptr;
    };

    const auto* actuel = findByDisplacement(0x904);
    ASSERT_NE(actuel, nullptr);
    EXPECT_EQ(actuel->address, knownWriteTargetAddress - 0x900 + 0x904);
    EXPECT_EQ(actuel->memBaseRegister, QStringLiteral("rsi"));
    EXPECT_EQ(actuel->inferredType, ValueType::Int32);

    const auto* cible = findByDisplacement(0x908);
    ASSERT_NE(cible, nullptr);
    EXPECT_EQ(cible->address, knownWriteTargetAddress - 0x900 + 0x908);
    EXPECT_EQ(cible->memBaseRegister, QStringLiteral("rsi"));
    EXPECT_EQ(cible->inferredType, ValueType::Int32);

    // L'instruction d'ecriture elle-meme (adresse == knownWriteTargetAddress)
    // ne doit jamais ressortir : l'appelant sait deja qu'elle "ne tient pas"
    // (c'est justement pourquoi il cherche une autre source).
    for (const auto& field : resolved) {
        EXPECT_NE(field.address, knownWriteTargetAddress);
    }
}

TEST(InstructionPatchSuggester, ResolveCandidateFieldAddressesIgnoresDifferentBaseRegister) {
    // mov ecx, [rax+0x10] (registre de base different de l'ecriture finale)
    // suivi de mov [rsi+0x900], edx : aucune valeur live pour rax n'est
    // disponible, le candidat doit rester non resolu plutot que de produire
    // une adresse fausse en melangeant les registres.
    const QByteArray bytes = QByteArray::fromHex(
        "8B4810"             // mov ecx, [rax+0x10]
        "899600090000");     // mov [rsi+0x900], edx
    const int targetOffset = bytes.size() - 6;

    const auto result = disassembleBackwardWindow(bytes, targetOffset);
    ASSERT_TRUE(result.success) << result.error.toStdString();

    if (result.instructions.last().decoder != "zydis") {
        GTEST_SKIP() << "memBaseRegister non renseigne sans decodeur Zydis (KILLENGINE_HAS_ZYDIS absent).";
    }

    const auto resolved = resolveCandidateFieldAddresses(result.instructions, 0x0000700012340900ULL);
    EXPECT_TRUE(resolved.isEmpty());
}

TEST(InstructionPatchSuggester, InferProbeValueTypeDetectsFloatAndDoubleMnemonics) {
    InstructionInfo intMov;
    intMov.mnemonicHint = "mov";
    intMov.disassembly = "mov eax, [rsi+0x908]";
    EXPECT_EQ(inferProbeValueType(intMov), ValueType::Int32);

    InstructionInfo floatMov;
    floatMov.mnemonicHint = "movss";
    floatMov.disassembly = "movss xmm0, [rsi+0x914]";
    EXPECT_EQ(inferProbeValueType(floatMov), ValueType::Float32);

    InstructionInfo doubleMov;
    doubleMov.mnemonicHint = "movsd";
    doubleMov.disassembly = "movsd xmm0, [rsi+0x914]";
    EXPECT_EQ(inferProbeValueType(doubleMov), ValueType::Float64);
}

TEST(InstructionPatchSuggester, DisassembleForwardWindowDecodesKnownShellcodeSequence) {
    // Meme sequence exacte (a valeurs d'immediat pres) que le shellcode fixe
    // documente dans docs/KILLENGINE_CLR_INSPECTOR_SPEC.md pour l'appel reel
    // d'un setter CLR (ApplicationController::callClrInstanceMethod) -- 8
    // instructions x64 simples, connues et stables, bon candidat de test pour
    // le desassemblage AVANT (chantier "desassemblage de methode").
    const QByteArray bytes = QByteArray::fromHex(
        "4883EC28"                   // sub rsp, 0x28
        "48B98877665544332211"       // mov rcx, 0x1122334455667788
        "48BA0000000000000000"       // mov rdx, 0
        "48B80000000000000000"       // mov rax, 0
        "FFD0"                       // call rax
        "4883C428"                   // add rsp, 0x28
        "33C0"                       // xor eax, eax
        "C3");                       // ret

    const auto instructions = disassembleForwardWindow(bytes, 8);

    ASSERT_EQ(instructions.size(), 8);
    for (const auto& instruction : instructions) {
        EXPECT_TRUE(instruction.success);
        EXPECT_GT(instruction.length, 0);
    }
    EXPECT_EQ(instructions[0].length, 4);   // sub rsp, 0x28
    EXPECT_EQ(instructions[4].length, 2);   // call rax
    EXPECT_EQ(instructions.last().length, 1); // ret

    if (instructions[1].decoder != "zydis") {
        // Le decodeur builtin (sans Zydis) traite "mov r64, imm64" (REX.W +
        // B8-BF) comme un imm32 (limite connue, pas introduite par ce
        // chantier) -- la longueur totale exacte n'est fiable qu'avec Zydis,
        // meme convention de skip que le reste de ce fichier de tests.
        GTEST_SKIP() << "Longueur exacte des mov r64,imm64 non fiable sans decodeur Zydis (KILLENGINE_HAS_ZYDIS absent).";
    }
    int totalLength = 0;
    for (const auto& instruction : instructions) {
        totalLength += instruction.length;
    }
    EXPECT_EQ(totalLength, bytes.size());
    EXPECT_EQ(instructions[1].length, 10);  // mov rcx, imm64
}

TEST(InstructionPatchSuggester, DisassembleForwardWindowRespectsInstructionCountLimit) {
    const QByteArray bytes = QByteArray::fromHex(
        "4883EC28"                   // sub rsp, 0x28
        "48B98877665544332211"       // mov rcx, imm64
        "48BA0000000000000000"       // mov rdx, imm64
        "48B80000000000000000"       // mov rax, imm64
        "FFD0"
        "4883C428"
        "33C0"
        "C3");

    const auto instructions = disassembleForwardWindow(bytes, 3);

    ASSERT_EQ(instructions.size(), 3);
    EXPECT_EQ(instructions[0].length, 4);
    if (instructions[1].decoder != "zydis") {
        GTEST_SKIP() << "Longueur exacte des mov r64,imm64 non fiable sans decodeur Zydis (KILLENGINE_HAS_ZYDIS absent).";
    }
    EXPECT_EQ(instructions[1].length, 10);
    EXPECT_EQ(instructions[2].length, 10);
}

TEST(InstructionPatchSuggester, DisassembleForwardWindowReturnsPartialResultOnTruncatedBuffer) {
    // Buffer coupe en plein milieu de la 3e instruction (mov rdx, imm64) :
    // les 2 premieres instructions completes doivent quand meme etre
    // retournees (pas de tout-ou-rien), la 3e est abandonnee proprement.
    const QByteArray fullBytes = QByteArray::fromHex(
        "4883EC28"                   // sub rsp, 0x28 (4 octets)
        "48B98877665544332211"       // mov rcx, imm64 (10 octets)
        "48BA0000000000000000");     // mov rdx, imm64 (10 octets, tronque ci-dessous)
    const QByteArray truncated = fullBytes.left(4 + 10 + 3); // 3 octets seulement de la 3e instruction

    const auto instructions = disassembleForwardWindow(truncated, 10);

    ASSERT_EQ(instructions.size(), 2);
    EXPECT_TRUE(instructions[0].success);
    EXPECT_TRUE(instructions[1].success);
    EXPECT_EQ(instructions[0].length, 4);
    if (instructions[1].decoder != "zydis") {
        GTEST_SKIP() << "Longueur exacte des mov r64,imm64 non fiable sans decodeur Zydis (KILLENGINE_HAS_ZYDIS absent).";
    }
    EXPECT_EQ(instructions[1].length, 10);
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
