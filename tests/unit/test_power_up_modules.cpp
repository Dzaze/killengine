#include "input/global_hotkey.h"
#include "freeze/freeze_manager.h"
#include "scanner/encrypted_scan.h"
#include "scanner/structure_analyzer.h"
#include "scripting/auto_assembler.h"

#include <gtest/gtest.h>

#include <QByteArray>

#include <algorithm>
#include <cstring>

using namespace killcore;

TEST(EncryptedScan, FindsXorValueWithBoundedKeySearch) {
    QByteArray buffer(16, '\0');
    const uint32_t displayValue = 1234;
    const uint32_t key = 0x42;
    const uint32_t storedValue = displayValue ^ key;
    std::memcpy(buffer.data() + 4, &storedValue, sizeof(storedValue));

    EncryptedScanOptions options;
    options.mode = EncryptedScanMode::XorKey;
    options.keySearchBits = 8;
    options.valueType = ValueType::UInt32;

    const auto result = scanEncryptedInBuffer(buffer, 0x1000, displayValue, options);

    ASSERT_TRUE(result.success) << result.error.toStdString();
    ASSERT_FALSE(result.matches.isEmpty());
    EXPECT_EQ(result.matches.front().address, 0x1004ULL);
    EXPECT_EQ(result.keyFound, key);
}

TEST(EncryptedScan, RejectsUnboundedKeySearch) {
    QByteArray buffer(16, '\0');

    EncryptedScanOptions options;
    options.mode = EncryptedScanMode::XorKey;
    options.keySearchBits = 64;
    options.valueType = ValueType::UInt64;

    const auto result = scanEncryptedInBuffer(buffer, 0x1000, 42, options);

    EXPECT_FALSE(result.success);
    EXPECT_FALSE(result.error.isEmpty());
}

TEST(EncryptedScan, EmptyGroupScanIsFailure) {
    QByteArray buffer(16, '\0');
    GroupScanOptions options;

    const auto result = scanGroupInBuffer(buffer, 0x1000, options);

    EXPECT_FALSE(result.success);
    EXPECT_FALSE(result.error.isEmpty());
}

TEST(AutoAssembler, RejectsUnsupportedSyntax) {
    const auto script = parseAutoAsmScript("this is not valid asm\n");

    EXPECT_FALSE(script.success);
    EXPECT_EQ(script.errorLine, 1);
    EXPECT_FALSE(script.error.isEmpty());
}

TEST(AutoAssembler, ParsesAllocAndLabelOnlyScript) {
    const auto script = parseAutoAsmScript("alloc(newmem, 256)\nlabel(returnhere)\nreturnhere:\n");

    ASSERT_TRUE(script.success) << script.error.toStdString();
    EXPECT_EQ(script.allocations.size(), 1);
    EXPECT_GE(script.labels.size(), 2);
}

TEST(AutoAssembler, CompilesRuntimePrimitivesAndRawBytes) {
    const auto script = parseAutoAsmScript("nop\nint3\ndb 90, 0xCC\ndd 0x11223344\nret\n");

    ASSERT_TRUE(script.success) << script.error.toStdString();
    const auto compiled = compileAutoAsmScript(script, 0x1000);

    ASSERT_TRUE(compiled.success) << compiled.error.toStdString();
    ASSERT_EQ(compiled.regions.size(), 1);
    EXPECT_EQ(compiled.regions.front().baseAddress, 0x1000ULL);
    EXPECT_EQ(compiled.regions.front().code.toHex(' ').toUpper(), QByteArray("90 CC 90 CC 44 33 22 11 C3"));
}

TEST(AutoAssembler, CompilesRelativeJumpToLocalLabel) {
    const auto script = parseAutoAsmScript("jmp done\nnop\ndone:\nret\n");

    ASSERT_TRUE(script.success) << script.error.toStdString();
    const auto compiled = compileAutoAsmScript(script, 0x1000);

    ASSERT_TRUE(compiled.success) << compiled.error.toStdString();
    ASSERT_EQ(compiled.regions.size(), 1);
    EXPECT_EQ(compiled.regions.front().code.toHex(' ').toUpper(), QByteArray("E9 01 00 00 00 90 C3"));
    ASSERT_EQ(compiled.labels.size(), 1);
    EXPECT_EQ(compiled.labels.front().name, QStringLiteral("done"));
    EXPECT_EQ(compiled.labels.front().address, 0x1006ULL);
}

TEST(AutoAssembler, CompilesRegisterRelativeMovWithImmediate) {
    // mov dword ptr [rax+8], 9999 -> C7 40 08 0F 27 00 00
    // (verifie independamment contre l'encodage manuel du meme motif utilise
    // par InstructionPatchSuggester.DecodesImmediateOperandOffsetForMemoryWrite
    // dans test_aob_scanner.cpp : C7 40 08 <imm32>)
    const auto script = parseAutoAsmScript("mov [rax+08], 9999\n");

    ASSERT_TRUE(script.success) << script.error.toStdString();
    const auto compiled = compileAutoAsmScript(script, 0x1000);

    ASSERT_TRUE(compiled.success) << compiled.error.toStdString();
    ASSERT_EQ(compiled.regions.size(), 1);
    EXPECT_EQ(compiled.regions.front().code.toHex(' ').toUpper(), QByteArray("C7 40 08 0F 27 00 00"));
}

TEST(AutoAssembler, CompilesExtendedRegisterAndSibRequiredBase) {
    // r8 (registre etendu) impose un prefixe REX.B ; rsp/r12 comme base
    // impose un octet SIB (ModRM.rm=100 ne peut pas designer directement rsp/r12).
    const auto r8Script = parseAutoAsmScript("mov [r8+4], 1\n");
    ASSERT_TRUE(r8Script.success) << r8Script.error.toStdString();
    const auto r8Compiled = compileAutoAsmScript(r8Script, 0x1000);
    ASSERT_TRUE(r8Compiled.success) << r8Compiled.error.toStdString();
    ASSERT_EQ(r8Compiled.regions.size(), 1);
    EXPECT_EQ(r8Compiled.regions.front().code.toHex(' ').toUpper(), QByteArray("41 C7 40 04 01 00 00 00"));

    const auto rspScript = parseAutoAsmScript("mov [rsp+4], 1\n");
    ASSERT_TRUE(rspScript.success) << rspScript.error.toStdString();
    const auto rspCompiled = compileAutoAsmScript(rspScript, 0x1000);
    ASSERT_TRUE(rspCompiled.success) << rspCompiled.error.toStdString();
    ASSERT_EQ(rspCompiled.regions.size(), 1);
    EXPECT_EQ(rspCompiled.regions.front().code.toHex(' ').toUpper(), QByteArray("C7 44 24 04 01 00 00 00"));
}

TEST(AutoAssembler, RejectsComplexAddressingUntilFullBackendExists) {
    // Adressage indexe ("[rax+rbx]") : hors de la portee volontairement
    // bornee de encodeMemImmMov (registre de base + deplacement uniquement).
    const auto script = parseAutoAsmScript("mov [rax+rbx], 9999\n");

    ASSERT_TRUE(script.success) << script.error.toStdString();
    const auto compiled = compileAutoAsmScript(script, 0x1000);

    EXPECT_FALSE(compiled.success);
    EXPECT_EQ(compiled.errorLine, 1);
    EXPECT_FALSE(compiled.error.isEmpty());
}

TEST(AutoAssembler, StillRejectsMnemonicsTheParserDoesNotRecognizeYet) {
    // add/sub/inc/dec/cmp/push/pop existent dans AutoAsmInstructionType (pour
    // l'API) mais parseAutoAsmScript() ne les reconnait pas encore comme
    // syntaxe valide — rejet des le parsing, pas seulement a la compilation.
    const auto script = parseAutoAsmScript("add [rax+8], 1\n");

    EXPECT_FALSE(script.success);
    EXPECT_EQ(script.errorLine, 1);
    EXPECT_FALSE(script.error.isEmpty());
}

TEST(AutoAssembler, RejectsEachArithmeticAndStackMnemonicCleanly) {
    // Meme verification que StillRejectsMnemonicsTheParserDoesNotRecognizeYet,
    // mais pour chaque mnemonique arithmetique/pile un par un (pas juste
    // "add" comme representant) : demande explicite propriétaire, "verifier
    // erreurs propres sur add/sub/cmp/push/pop non supportes" -- s'assurer
    // qu'aucun d'eux ne passe silencieusement le parsing ou ne fait planter
    // le compilateur faute de case dans le switch (AutoAsmInstructionType a
    // bien une entree pour chacun, seul le token textuel n'est pas reconnu).
    const QStringList unsupportedLines = {
        "add [rax+8], 1",
        "sub [rax+8], 1",
        "cmp [rax+8], 1",
        "push 1",
        "pop [rax+8]",
        "inc [rax+8]",
        "dec [rax+8]",
    };
    for (const QString& line : unsupportedLines) {
        const auto script = parseAutoAsmScript(line + "\n");
        EXPECT_FALSE(script.success) << line.toStdString() << " a ete accepte par le parser a tort.";
        EXPECT_EQ(script.errorLine, 1) << line.toStdString();
        EXPECT_FALSE(script.error.isEmpty()) << line.toStdString();
    }
}

TEST(AutoAssembler, AcceptsEverySupportedRuntimeMnemonic) {
    // Symetrique du test ci-dessus : verifie explicitement chaque mnemonique
    // que compileAutoAsmScript sait reellement encoder (doc du header
    // auto_assembler.h : "nop, ret, int3, db/de/dd, jmp/call/je/jne vers
    // adresse absolue ou label local, mov [registre64+/-deplacement], imm32"),
    // pas seulement un sous-ensemble ad hoc deja couvert ailleurs.
    const QStringList supportedLines = {
        "nop",
        "ret",
        "int3",
        "db 90",
        "de 90",
        "dd 0x11223344",
        "jmp done\ndone:",
        "call done\ndone:",
        "je done\ndone:",
        "jne done\ndone:",
        "mov [rax+8], 1",
    };
    for (const QString& scriptText : supportedLines) {
        const auto script = parseAutoAsmScript(scriptText + "\n");
        ASSERT_TRUE(script.success) << scriptText.toStdString() << " : " << script.error.toStdString();
        const auto compiled = compileAutoAsmScript(script, 0x1000);
        EXPECT_TRUE(compiled.success) << scriptText.toStdString() << " : " << compiled.error.toStdString();
    }
}

TEST(AutoAssembler, ParsesModuleRelativeBlockOpener) {
    const auto script = parseAutoAsmScript("\"game.exe\"+0x1000:\nnop\n");

    ASSERT_TRUE(script.success) << script.error.toStdString();
    ASSERT_EQ(script.instructions.size(), 2);
    EXPECT_EQ(script.instructions.front().type, AutoAsmInstructionType::ModuleLabel);
    EXPECT_EQ(script.instructions.front().target, QStringLiteral("game.exe"));
    EXPECT_EQ(script.instructions.front().offset, 0x1000);
}

TEST(AutoAssembler, RejectsModuleBlockWhenModuleUnresolved) {
    // Sans contexte (aucun module connu, ex: script compile hors ligne ou
    // processus non attache), un bloc "module"+offset: doit echouer
    // proprement plutot que de silencieusement utiliser une adresse bidon.
    const auto script = parseAutoAsmScript("\"missing.dll\"+0x10:\nnop\n");
    ASSERT_TRUE(script.success) << script.error.toStdString();

    const auto compiled = compileAutoAsmScript(script);

    EXPECT_FALSE(compiled.success);
    EXPECT_EQ(compiled.errorLine, 1);
    EXPECT_FALSE(compiled.error.isEmpty());
}

TEST(AutoAssembler, CompilesCeStyleTwoRegionScript) {
    // Pattern CE classique complet : bloc alloue (trampoline "newmem") +
    // site EXISTANT du processus cible ("game.exe"+0x1000) redirige vers
    // lui, avec un label ("returnhere") partage entre les deux regions —
    // exactement ce qu'executeAutoAsmScript doit maintenant savoir ecrire en
    // deux passes WriteProcessMemory distinctes.
    const QString scriptText = QStringLiteral(
        "alloc(newmem, 256)\n"
        "label(returnhere)\n"
        "\n"
        "newmem:\n"
        "mov [rax+08], 9999\n"
        "jmp returnhere\n"
        "\n"
        "\"game.exe\"+0x1000:\n"
        "jmp newmem\n"
        "nop\n"
        "returnhere:\n"
        "ret\n");

    const auto script = parseAutoAsmScript(scriptText);
    ASSERT_TRUE(script.success) << script.error.toStdString();

    // Adresses volontairement proches (< 2 Go d'ecart) : un jmp rel32 ne
    // couvre que +/-2 Go, contrainte reelle de l'encodage E9 deja respectee
    // ailleurs (cf. le check "Branch target is outside rel32 range"). Une
    // allocation VirtualAllocEx(nullptr, ...) tombe parfois bien au-dela de
    // ca du module cible — hors de portee de ce compilateur volontairement
    // borne (function_hook.cpp gere deja le jmp 14 octets pour ce cas via
    // generateJumpShellcode, chemin distinct de l'auto-assembleur).
    AutoAsmCompileContext context;
    context.allocationAddresses.insert("newmem", 0x140100000ULL);
    context.moduleBaseAddresses.insert("game.exe", 0x140000000ULL);

    const auto compiled = compileAutoAsmScript(script, 0, context);
    ASSERT_TRUE(compiled.success) << compiled.error.toStdString();
    ASSERT_EQ(compiled.regions.size(), 2);

    const auto& newmemRegion = compiled.regions[0];
    const auto& siteRegion = compiled.regions[1];
    EXPECT_EQ(newmemRegion.baseAddress, 0x140100000ULL);
    EXPECT_EQ(siteRegion.baseAddress, 0x140001000ULL);

    // newmem: "mov [rax+08], 9999" (7o, encodage deja verifie octet par octet
    // par CompilesRegisterRelativeMovWithImmediate) suivi de "jmp returnhere" (5o).
    ASSERT_EQ(newmemRegion.code.size(), 12);
    EXPECT_EQ(newmemRegion.code.left(7).toHex(' ').toUpper(), QByteArray("C7 40 08 0F 27 00 00"));
    EXPECT_EQ(static_cast<uint8_t>(newmemRegion.code.at(7)), 0xE9);

    // site: "jmp newmem" (5o) + "nop" (1o) + "ret" (1o, meme region que
    // returnhere: puisque ce label n'est PAS lie a un alloc()).
    ASSERT_EQ(siteRegion.code.size(), 7);
    EXPECT_EQ(static_cast<uint8_t>(siteRegion.code.at(0)), 0xE9);
    EXPECT_EQ(static_cast<uint8_t>(siteRegion.code.at(5)), 0x90);
    EXPECT_EQ(static_cast<uint8_t>(siteRegion.code.at(6)), 0xC3);

    const auto returnhereLabel = std::find_if(compiled.labels.begin(), compiled.labels.end(), [](const auto& l) {
        return l.name == QStringLiteral("returnhere");
    });
    ASSERT_NE(returnhereLabel, compiled.labels.end());
    EXPECT_EQ(returnhereLabel->address, siteRegion.baseAddress + 5 + 1);

    // "jmp returnhere" (newmemRegion, offset 7) code bien rel32 = target - (adresse_jmp + 5).
    const uint64_t jmpReturnhereAddress = newmemRegion.baseAddress + 7;
    const int32_t expectedRel1 = static_cast<int32_t>(
        static_cast<int64_t>(returnhereLabel->address) - static_cast<int64_t>(jmpReturnhereAddress + 5));
    int32_t actualRel1 = 0;
    std::memcpy(&actualRel1, newmemRegion.code.constData() + 8, 4);
    EXPECT_EQ(actualRel1, expectedRel1);

    // "jmp newmem" (siteRegion, offset 0) code bien rel32 = target - (adresse_jmp + 5).
    const int32_t expectedRel2 = static_cast<int32_t>(
        static_cast<int64_t>(newmemRegion.baseAddress) - static_cast<int64_t>(siteRegion.baseAddress + 5));
    int32_t actualRel2 = 0;
    std::memcpy(&actualRel2, siteRegion.code.constData() + 1, 4);
    EXPECT_EQ(actualRel2, expectedRel2);
}

TEST(GlobalHotkey, RoundTripsReadableFunctionKey) {
    const auto combo = HotkeyCombo::fromString("Ctrl+Shift+F3");

    EXPECT_TRUE(combo.ctrl);
    EXPECT_TRUE(combo.shift);
    EXPECT_FALSE(combo.alt);
    EXPECT_EQ(combo.toString(), QStringLiteral("Ctrl+Shift+F3"));
}

TEST(GlobalHotkey, DoesNotInstallNativeFilterBeforeRegistration) {
    GlobalHotkeyManager manager;

    EXPECT_FALSE(manager.isNativeFilterInstalled());
    EXPECT_TRUE(manager.registeredHotkeys().isEmpty());
    EXPECT_FALSE(manager.nativeEventFilter("not_windows", nullptr, nullptr));
}

TEST(FreezeManager, KeepsPollingAndBreakpointEntriesSeparated) {
    FreezeManager manager;
    manager.setEntry(0x1000, ValueType::Int32, QByteArray::fromHex("01000000"), FreezeMode::Polling);
    manager.setEntry(0x2000, ValueType::Int32, QByteArray::fromHex("02000000"), FreezeMode::HardwareBreakpoint);

    EXPECT_FALSE(manager.isEmpty());
    EXPECT_TRUE(manager.hasMode(FreezeMode::Polling));
    EXPECT_TRUE(manager.hasMode(FreezeMode::HardwareBreakpoint));
    EXPECT_EQ(manager.entriesForMode(FreezeMode::Polling).size(), 1);
    EXPECT_EQ(manager.entriesForMode(FreezeMode::HardwareBreakpoint).size(), 1);

    manager.removeByMode(FreezeMode::HardwareBreakpoint);

    EXPECT_TRUE(manager.hasMode(FreezeMode::Polling));
    EXPECT_FALSE(manager.hasMode(FreezeMode::HardwareBreakpoint));
    ASSERT_EQ(manager.entriesForMode(FreezeMode::Polling).size(), 1);
    EXPECT_EQ(manager.entriesForMode(FreezeMode::Polling).front().address, 0x1000ULL);
}

TEST(FreezeManager, DoesNotFlagUnstableWhileValueHolds) {
    FreezeManager manager;
    manager.setEntry(0x1000, ValueType::Int32, QByteArray::fromHex("01000000"), FreezeMode::Polling);

    for (int i = 0; i < 50; ++i) {
        EXPECT_FALSE(manager.recordPollTick(0x1000, /*valueMatchedBeforeRewrite=*/true));
    }

    ASSERT_EQ(manager.entries().size(), 1);
    const auto& entry = manager.entries().front();
    EXPECT_EQ(entry.consecutiveDriftTicks, 0);
    EXPECT_EQ(entry.totalDriftTicks, 0);
    EXPECT_EQ(entry.totalTicks, 50);
    EXPECT_FALSE(entry.flaggedUnstable);
}

TEST(FreezeManager, FlagsUnstableAfterConsecutiveDriftTicksAndOnlyOnce) {
    FreezeManager manager;
    manager.setEntry(0x1000, ValueType::Int32, QByteArray::fromHex("01000000"), FreezeMode::Polling);

    bool crossed = false;
    for (int i = 0; i < kFreezePollDriftThreshold - 1; ++i) {
        crossed = manager.recordPollTick(0x1000, /*valueMatchedBeforeRewrite=*/false);
        EXPECT_FALSE(crossed) << "ne doit pas se declencher avant le seuil (tick " << i << ")";
    }

    // Le tick qui atteint exactement le seuil doit declencher la notification.
    crossed = manager.recordPollTick(0x1000, /*valueMatchedBeforeRewrite=*/false);
    EXPECT_TRUE(crossed);

    // Les ticks suivants restent en derive mais ne re-notifient pas (flaggedUnstable=true).
    for (int i = 0; i < 10; ++i) {
        EXPECT_FALSE(manager.recordPollTick(0x1000, /*valueMatchedBeforeRewrite=*/false));
    }

    ASSERT_EQ(manager.entries().size(), 1);
    const auto& entry = manager.entries().front();
    EXPECT_TRUE(entry.flaggedUnstable);
    EXPECT_EQ(entry.consecutiveDriftTicks, kFreezePollDriftThreshold + 10);
    EXPECT_EQ(entry.totalDriftTicks, kFreezePollDriftThreshold + 10);
    EXPECT_EQ(entry.totalTicks, kFreezePollDriftThreshold + 10);
}

TEST(FreezeManager, RecoveringResetsConsecutiveCounterButKeepsHistoricalTotals) {
    FreezeManager manager;
    manager.setEntry(0x1000, ValueType::Int32, QByteArray::fromHex("01000000"), FreezeMode::Polling);

    for (int i = 0; i < 3; ++i) {
        manager.recordPollTick(0x1000, /*valueMatchedBeforeRewrite=*/false);
    }
    // Une reprise remet le compteur CONSECUTIF a zero (pas encore instable),
    // mais l'historique total de derive reste pour le calcul du taux de tenue.
    manager.recordPollTick(0x1000, /*valueMatchedBeforeRewrite=*/true);

    ASSERT_EQ(manager.entries().size(), 1);
    const auto& entry = manager.entries().front();
    EXPECT_EQ(entry.consecutiveDriftTicks, 0);
    EXPECT_EQ(entry.totalDriftTicks, 3);
    EXPECT_EQ(entry.totalTicks, 4);
    EXPECT_FALSE(entry.flaggedUnstable);
}

TEST(FreezeManager, ReArmingSameAddressResetsReliabilityState) {
    FreezeManager manager;
    manager.setEntry(0x1000, ValueType::Int32, QByteArray::fromHex("01000000"), FreezeMode::Polling);
    for (int i = 0; i < kFreezePollDriftThreshold; ++i) {
        manager.recordPollTick(0x1000, /*valueMatchedBeforeRewrite=*/false);
    }
    ASSERT_TRUE(manager.entries().front().flaggedUnstable);

    // Reactiver un freeze sur la meme adresse (nouvelle valeur cible, par ex.)
    // doit repartir sur un diagnostic propre plutot que d'heriter de l'ancien.
    manager.setEntry(0x1000, ValueType::Int32, QByteArray::fromHex("02000000"), FreezeMode::Polling);

    ASSERT_EQ(manager.entries().size(), 1);
    const auto& entry = manager.entries().front();
    EXPECT_FALSE(entry.flaggedUnstable);
    EXPECT_EQ(entry.consecutiveDriftTicks, 0);
    EXPECT_EQ(entry.totalDriftTicks, 0);
    EXPECT_EQ(entry.totalTicks, 0);
}

TEST(StructureAnalyzer, DetectsUnalignedStringsAndWideNumericTypes) {
    QByteArray buffer(64, '\0');
    const char text[] = "gold";
    const uint16_t u16 = 65000;
    const uint64_t u64 = 0xF000000000000123ULL;
    const double f64 = 123.5;

    std::memcpy(buffer.data() + 3, text, sizeof(text));
    std::memcpy(buffer.data() + 10, &u16, sizeof(u16));
    std::memcpy(buffer.data() + 16, &u64, sizeof(u64));
    std::memcpy(buffer.data() + 24, &f64, sizeof(f64));

    const auto result = analyzeStructure(buffer, 0x5000);

    ASSERT_TRUE(result.success) << result.error.toStdString();

    auto hasField = [&](int offset, FieldType type) {
        return std::any_of(result.fields.cbegin(), result.fields.cend(), [&](const StructureField& field) {
            return field.offset == offset && field.type == type;
        });
    };

    EXPECT_TRUE(hasField(3, FieldType::AsciiString));
    EXPECT_TRUE(hasField(10, FieldType::UInt16));
    EXPECT_TRUE(hasField(16, FieldType::UInt64));
    EXPECT_TRUE(hasField(24, FieldType::Float64));
}

TEST(StructureAnalyzer, DiffMarksChangedFieldsWithRealFieldSizes) {
    QByteArray previous(32, '\0');
    QByteArray current(32, '\0');
    const uint64_t oldValue = 100;
    const uint64_t newValue = 200;
    std::memcpy(previous.data() + 8, &oldValue, sizeof(oldValue));
    std::memcpy(current.data() + 8, &newValue, sizeof(newValue));

    const auto previousResult = analyzeStructure(previous, 0x6000);
    const auto diff = diffStructure(previousResult, current);

    ASSERT_TRUE(diff.success) << diff.error.toStdString();

    const auto it = std::find_if(diff.fields.cbegin(), diff.fields.cend(), [](const StructureField& field) {
        return field.offset == 8 && field.type == FieldType::Int64;
    });
    ASSERT_NE(it, diff.fields.cend());
    EXPECT_TRUE(it->changed);
}

TEST(StructureAnalyzer, InfersInstanceDeltaFromMatchingFields) {
    killcore::StructureInstanceDeltaOptions options;
    options.beforeCount = 1;
    options.afterCount = 3;

    const auto result = inferStructureInstanceDelta(
        0x1000,
        0x1018,
        0x1200,
        0x1218,
        options);

    ASSERT_TRUE(result.success) << result.error.toStdString();
    EXPECT_TRUE(result.compatibleLayout);
    EXPECT_EQ(result.fieldOffsetA, 0x18);
    EXPECT_EQ(result.fieldOffsetB, 0x18);
    EXPECT_EQ(result.fieldOffsetDelta, 0);
    EXPECT_EQ(result.instanceDelta, 0x200);
    EXPECT_EQ(result.fieldAddressDelta, 0x200);
    ASSERT_EQ(result.candidates.size(), 5);
    EXPECT_EQ(result.candidates.front().relativeIndex, -1);
    EXPECT_EQ(result.candidates.front().baseAddress, 0x0E00);
    EXPECT_EQ(result.candidates.front().fieldAddress, 0x0E18);
    EXPECT_EQ(result.candidates[1].relativeIndex, 0);
    EXPECT_TRUE(result.candidates[1].inputInstance);
    EXPECT_EQ(result.candidates[2].relativeIndex, 1);
    EXPECT_TRUE(result.candidates[2].inputInstance);
    EXPECT_EQ(result.candidates.back().baseAddress, 0x1600);
    EXPECT_EQ(result.candidates.back().fieldAddress, 0x1618);
}

TEST(StructureAnalyzer, ReportsMismatchedFieldOffsetsWithoutPredictions) {
    const auto result = inferStructureInstanceDelta(
        0x3000,
        0x3020,
        0x3400,
        0x3428);

    ASSERT_TRUE(result.success) << result.error.toStdString();
    EXPECT_FALSE(result.compatibleLayout);
    EXPECT_EQ(result.fieldOffsetA, 0x20);
    EXPECT_EQ(result.fieldOffsetB, 0x28);
    EXPECT_EQ(result.fieldOffsetDelta, 8);
    EXPECT_TRUE(result.warning.contains("Offsets"));
    EXPECT_TRUE(result.candidates.isEmpty());
}
