#include "profiles/ghidra_bridge.h"

#include <gtest/gtest.h>

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

using namespace killcore;

namespace {

Profile buildGhidraProfile() {
    Profile profile;
    profile.gameName = "Ghidra Game";
    profile.executableName = "game.exe";
    profile.executableHash = "abc123";

    ProfileTarget money;
    money.name = "Money";
    money.type = ValueType::Int32;
    money.locator.kind = LocatorKind::ModuleOffset;
    money.locator.module = "game.exe";
    money.locator.offset = 0x1234;
    money.locator.lastAddress = 0x7ff600001234ULL;
    money.description = "Runtime money target";
    profile.targets.append(money);

    ProfileTarget health;
    health.name = "Health pointer";
    health.type = ValueType::Int32;
    health.locator.kind = LocatorKind::PointerChain;
    health.locator.pointerChain.module = "game.exe";
    health.locator.pointerChain.baseOffset = 0x2000;
    health.locator.pointerChain.offsets = {0x10, 0x8};
    health.dependsOn = {"Money"};
    profile.targets.append(health);

    ProfileCodePatch patch;
    patch.name = "No damage";
    patch.module = "game.exe";
    patch.moduleOffset = 0x3456;
    patch.aobPattern = "89 81 ?? ?? 00 00";
    patch.patchBytes = "90 90 90 90 90 90";
    patch.originalBytes = "89 81 10 01 00 00";
    patch.description = "Damage write site";
    patch.signatureScore = 92;
    patch.signatureLevel = "strong";
    patch.trainerSafe = true;
    patch.signatureMatches = 1;
    profile.patches.append(patch);

    return profile;
}

} // namespace

TEST(GhidraBridge, ExportsProfileArtifactsForGhidra) {
    const Profile profile = buildGhidraProfile();

    const QJsonObject exported = exportGhidraArtifacts(profile);

    EXPECT_EQ(exported.value("format").toString(), QStringLiteral("killengine.ghidra_artifacts"));
    EXPECT_EQ(exported.value("formatVersion").toInt(), 1);
    EXPECT_EQ(exported.value("profileGameName").toString(), profile.gameName);

    const QJsonArray artifacts = exported.value("artifacts").toArray();
    ASSERT_EQ(artifacts.size(), 3);

    const QJsonObject money = artifacts[0].toObject();
    EXPECT_EQ(money.value("category").toString(), QStringLiteral("target"));
    EXPECT_EQ(money.value("module").toString(), QStringLiteral("game.exe"));
    EXPECT_EQ(money.value("offset").toString(), QStringLiteral("1234"));
    EXPECT_TRUE(money.value("ghidraAddressable").toBool());

    const QJsonObject pointer = artifacts[1].toObject();
    EXPECT_EQ(pointer.value("locatorKind").toString(), QStringLiteral("pointer_chain"));
    EXPECT_EQ(pointer.value("pointerChain").toObject().value("baseOffset").toString(), QStringLiteral("2000"));
    EXPECT_EQ(pointer.value("dependsOn").toArray()[0].toString(), QStringLiteral("Money"));

    const QJsonObject patch = artifacts[2].toObject();
    EXPECT_EQ(patch.value("category").toString(), QStringLiteral("patch"));
    EXPECT_EQ(patch.value("aobPattern").toString(), QStringLiteral("89 81 ?? ?? 00 00"));
    EXPECT_EQ(patch.value("signatureQuality").toObject().value("level").toString(), QStringLiteral("strong"));
}

TEST(GhidraBridge, GeneratesScriptWithEmbeddedArtifacts) {
    const QJsonObject exported = exportGhidraArtifacts(buildGhidraProfile());

    const QString script = generateGhidraImportScript(exported);

    EXPECT_TRUE(script.contains("KillEngine -> Ghidra bridge import"));
    EXPECT_TRUE(script.contains("json.loads"));
    EXPECT_TRUE(script.contains("killengine.ghidra_artifacts"));
    EXPECT_TRUE(script.contains("KillEngine AOB"));
}

TEST(GhidraBridge, ImportsJsonSymbolsIntoMatchingTargetsAndPatches) {
    Profile profile = buildGhidraProfile();
    QJsonObject root;
    QJsonArray symbols;
    symbols.append(QJsonObject{
        {"module", "game.exe"},
        {"offset", "1234"},
        {"name", "PlayerMoney"},
        {"comment", "Named in Ghidra"},
    });
    symbols.append(QJsonObject{
        {"module", "game.exe"},
        {"offset", "3456"},
        {"name", "ApplyDamage"},
        {"comment", "Patch candidate"},
    });
    root["symbols"] = symbols;

    GhidraSymbolImportResult result;
    QString error;
    ASSERT_TRUE(importGhidraSymbols(&profile, QJsonDocument(root).toJson(), &result, &error)) << error.toStdString();

    EXPECT_EQ(result.symbolsRead, 2);
    EXPECT_EQ(result.targetsUpdated, 1);
    EXPECT_EQ(result.patchesUpdated, 1);
    EXPECT_EQ(profile.targets[0].name, QStringLiteral("Money"));
    EXPECT_EQ(profile.targets[0].ghidraSymbol, QStringLiteral("PlayerMoney"));
    EXPECT_TRUE(profile.targets[0].description.contains("[Ghidra] PlayerMoney"));
    EXPECT_EQ(profile.patches[0].ghidraSymbol, QStringLiteral("ApplyDamage"));
    EXPECT_TRUE(profile.patches[0].description.contains("Patch candidate"));
}

TEST(GhidraBridge, ImportsCsvSymbolsAndReportsUnmatchedRows) {
    Profile profile = buildGhidraProfile();
    const QByteArray csv =
        "module,offset,name,comment\n"
        "game.exe,0x2000,PlayerBase,Pointer anchor\n"
        "other.exe,0x9999,OtherThing,No match\n";

    GhidraSymbolImportResult result;
    QString error;
    ASSERT_TRUE(importGhidraSymbols(&profile, csv, &result, &error)) << error.toStdString();

    EXPECT_EQ(result.symbolsRead, 2);
    EXPECT_EQ(result.targetsUpdated, 1);
    EXPECT_EQ(result.unmatched, 1);
    EXPECT_EQ(profile.targets[1].ghidraSymbol, QStringLiteral("PlayerBase"));
    EXPECT_EQ(profile.targets[1].name, QStringLiteral("Health pointer"));
}

TEST(GhidraBridge, ImportsAbsoluteGhidraAddressWhenImageBaseIsProvided) {
    Profile profile = buildGhidraProfile();
    QJsonObject root;
    root["imageBase"] = "0x140000000";
    QJsonArray symbols;
    symbols.append(QJsonObject{
        {"module", "game.exe"},
        {"address", "0x140003456"},
        {"name", "ApplyDamageAbsolute"},
        {"comment", "VA exported from Ghidra"},
    });
    root["symbols"] = symbols;

    GhidraSymbolImportResult result;
    QString error;
    ASSERT_TRUE(importGhidraSymbols(&profile, QJsonDocument(root).toJson(), &result, &error)) << error.toStdString();

    EXPECT_EQ(result.symbolsRead, 1);
    EXPECT_EQ(result.patchesUpdated, 1);
    EXPECT_EQ(profile.patches[0].ghidraSymbol, QStringLiteral("ApplyDamageAbsolute"));
    EXPECT_TRUE(profile.patches[0].description.contains("VA exported from Ghidra"));
}
