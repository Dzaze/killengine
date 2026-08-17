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
    EXPECT_EQ(compiled.code.toHex(' ').toUpper(), QByteArray("90 CC 90 CC 44 33 22 11 C3"));
}

TEST(AutoAssembler, CompilesRelativeJumpToLocalLabel) {
    const auto script = parseAutoAsmScript("jmp done\nnop\ndone:\nret\n");

    ASSERT_TRUE(script.success) << script.error.toStdString();
    const auto compiled = compileAutoAsmScript(script, 0x1000);

    ASSERT_TRUE(compiled.success) << compiled.error.toStdString();
    EXPECT_EQ(compiled.code.toHex(' ').toUpper(), QByteArray("E9 01 00 00 00 90 C3"));
    ASSERT_EQ(compiled.labels.size(), 1);
    EXPECT_EQ(compiled.labels.front().name, QStringLiteral("done"));
    EXPECT_EQ(compiled.labels.front().address, 0x1006ULL);
}

TEST(AutoAssembler, RejectsComplexInstructionsUntilFullBackendExists) {
    const auto script = parseAutoAsmScript("mov [rax+08], 9999\n");

    ASSERT_TRUE(script.success) << script.error.toStdString();
    const auto compiled = compileAutoAsmScript(script, 0x1000);

    EXPECT_FALSE(compiled.success);
    EXPECT_EQ(compiled.errorLine, 1);
    EXPECT_FALSE(compiled.error.isEmpty());
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
