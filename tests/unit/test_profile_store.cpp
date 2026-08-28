// Audit de persistance des profils (.keprofile) -- demande explicite
// propriétaire (liste de 7 chantiers, point 7) : "vérifier que profils,
// Trainer, scripts Lua, CLR targets, patchs et dépendances se rechargent
// correctement... tests unitaires de round-trip JSON, cas de données
// anciennes/manquantes". Portée de ce fichier : core/profiles/profile_store.*
// (targets/patches/autoAsmScripts/luaScripts), qui n'avait jusqu'ici AUCUN
// test dédié -- vérifié avant d'écrire quoi que ce soit (grep sur tests/).
// Le nettoyage des dépendances Trainer mortes (dependsOn) est une logique
// JS séparée (ui/src/stores/trainerDependencies.ts), déjà couverte par
// scripts/test-trainer-dependencies.ps1 -- pas dupliqué ici.

#include "profiles/profile_store.h"

#include <gtest/gtest.h>

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QTextStream>

using namespace killcore;

namespace {

Profile buildFullProfile() {
    Profile profile;
    profile.gameName = "KillEngine Test Target";
    profile.executableName = "KillEngineTestTarget.exe";
    profile.executableHash = "deadbeefcafef00d";

    ProfileTarget moduleOffsetTarget;
    moduleOffsetTarget.name = "Money";
    moduleOffsetTarget.type = ValueType::Int32;
    moduleOffsetTarget.locator.kind = LocatorKind::ModuleOffset;
    moduleOffsetTarget.locator.module = "KillEngineTestTarget.exe";
    moduleOffsetTarget.locator.offset = 0x3F2A0;
    moduleOffsetTarget.locator.lastAddress = 0x7FF612345678ULL;
    moduleOffsetTarget.description = "Money counter";
    moduleOffsetTarget.ghidraSymbol = "PlayerMoney";
    moduleOffsetTarget.ghidraNote = "Imported symbol";
    profile.targets.append(moduleOffsetTarget);

    ProfileTarget absoluteTarget;
    absoluteTarget.name = "DebugOnly";
    absoluteTarget.type = ValueType::Float32;
    absoluteTarget.locator.kind = LocatorKind::Absolute;
    absoluteTarget.locator.lastAddress = 0x7FF6AAAABBBBULL;
    profile.targets.append(absoluteTarget);

    ProfileTarget pointerChainTarget;
    pointerChainTarget.name = "PlayerHealth";
    pointerChainTarget.type = ValueType::Int32;
    pointerChainTarget.locator.kind = LocatorKind::PointerChain;
    pointerChainTarget.locator.pointerChain.module = "KillEngineTestTarget.exe";
    pointerChainTarget.locator.pointerChain.baseOffset = 0x12345;
    pointerChainTarget.locator.pointerChain.offsets = {0x10, 0x28, 0x8};
    pointerChainTarget.dependsOn = {"Money", "ManagedHealth"};
    profile.targets.append(pointerChainTarget);

    ProfileTarget clrFieldTarget;
    clrFieldTarget.name = "ManagedHealth";
    clrFieldTarget.type = ValueType::Int32;
    clrFieldTarget.locator.kind = LocatorKind::ClrField;
    clrFieldTarget.locator.clrField.typeSubstring = "Player";
    clrFieldTarget.locator.clrField.identityField = "Id";
    clrFieldTarget.locator.clrField.identityValue = "42";
    clrFieldTarget.locator.clrField.targetField = "Health";
    profile.targets.append(clrFieldTarget);

    ProfileCodePatch fullPatch;
    fullPatch.name = "Infinite Ammo";
    fullPatch.module = "KillEngineTestTarget.exe";
    fullPatch.moduleOffset = 0x5234;
    fullPatch.aobPattern = "48 83 EC ??";
    fullPatch.patchBytes = "90 90 90 90";
    fullPatch.originalBytes = "48 83 EC 28";
    fullPatch.disassembly = "sub rsp, 28";
    fullPatch.riskLevel = "medium";
    fullPatch.description = "NOPs the ammo decrement";
    fullPatch.ghidraSymbol = "ApplyAmmoCost";
    fullPatch.ghidraNote = "Named in Ghidra";
    fullPatch.signatureScore = 87;
    fullPatch.signatureLevel = "good";
    fullPatch.signatureWarning = "";
    fullPatch.signatureFixedBytes = 6;
    fullPatch.signatureWildcardBytes = 1;
    fullPatch.signatureUniqueFixedBytes = 6;
    fullPatch.signatureFixedRatio = 0.857;
    fullPatch.trainerSafe = true;
    fullPatch.signatureMatches = 1;
    profile.patches.append(fullPatch);

    ProfileCodePatch minimalPatch;
    minimalPatch.name = "Minimal";
    minimalPatch.module = "KillEngineTestTarget.exe";
    minimalPatch.moduleOffset = 0x1000;
    minimalPatch.aobPattern = "90";
    minimalPatch.patchBytes = "CC";
    profile.patches.append(minimalPatch);

    ProfileAutoAsmScript asmScript;
    asmScript.name = "Trampoline";
    asmScript.scriptText = "alloc(newmem, 256)\nnewmem:\nmov [rax+8], 1\n";
    asmScript.description = "Test trampoline";
    asmScript.riskLevel = "high";
    profile.autoAsmScripts.append(asmScript);

    ProfileLuaScript luaScript;
    luaScript.name = "Ping helper";
    luaScript.scriptText = "local ke = require('killengine')\nprint(ke.ping('hi'))\n";
    luaScript.description = "Smoke test script";
    luaScript.savedAtEpochMs = 1735000000000LL;
    profile.luaScripts.append(luaScript);

    return profile;
}

} // namespace

TEST(ProfileStore, RoundTripsFullProfileExactly) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString path = dir.filePath("full.keprofile");

    const Profile original = buildFullProfile();
    ASSERT_TRUE(ProfileStore::save(original, path));

    Profile loaded;
    ASSERT_TRUE(ProfileStore::load(path, &loaded));

    EXPECT_EQ(loaded.gameName, original.gameName);
    EXPECT_EQ(loaded.executableName, original.executableName);
    EXPECT_EQ(loaded.executableHash, original.executableHash);

    ASSERT_EQ(loaded.targets.size(), original.targets.size());
    for (int i = 0; i < original.targets.size(); ++i) {
        const auto& a = original.targets[i];
        const auto& b = loaded.targets[i];
        EXPECT_EQ(b.name, a.name) << i;
        EXPECT_EQ(static_cast<int>(b.type), static_cast<int>(a.type)) << i;
        EXPECT_EQ(b.description, a.description) << i;
        EXPECT_EQ(static_cast<int>(b.locator.kind), static_cast<int>(a.locator.kind)) << i;
        EXPECT_EQ(b.locator.module, a.locator.module) << i;
        EXPECT_EQ(b.locator.offset, a.locator.offset) << i;
        EXPECT_EQ(b.locator.lastAddress, a.locator.lastAddress) << i;
        EXPECT_EQ(b.locator.pointerChain.module, a.locator.pointerChain.module) << i;
        EXPECT_EQ(b.locator.pointerChain.baseOffset, a.locator.pointerChain.baseOffset) << i;
        EXPECT_EQ(b.locator.pointerChain.offsets, a.locator.pointerChain.offsets) << i;
        EXPECT_EQ(b.locator.clrField.typeSubstring, a.locator.clrField.typeSubstring) << i;
        EXPECT_EQ(b.locator.clrField.identityField, a.locator.clrField.identityField) << i;
        EXPECT_EQ(b.locator.clrField.identityValue, a.locator.clrField.identityValue) << i;
        EXPECT_EQ(b.locator.clrField.targetField, a.locator.clrField.targetField) << i;
        EXPECT_EQ(b.dependsOn, a.dependsOn) << i;
        EXPECT_EQ(b.ghidraSymbol, a.ghidraSymbol) << i;
        EXPECT_EQ(b.ghidraNote, a.ghidraNote) << i;
    }

    ASSERT_EQ(loaded.patches.size(), original.patches.size());
    const auto& origFullPatch = original.patches[0];
    const auto& loadedFullPatch = loaded.patches[0];
    EXPECT_EQ(loadedFullPatch.name, origFullPatch.name);
    EXPECT_EQ(loadedFullPatch.module, origFullPatch.module);
    EXPECT_EQ(loadedFullPatch.moduleOffset, origFullPatch.moduleOffset);
    EXPECT_EQ(loadedFullPatch.aobPattern, origFullPatch.aobPattern);
    EXPECT_EQ(loadedFullPatch.patchBytes, origFullPatch.patchBytes);
    EXPECT_EQ(loadedFullPatch.originalBytes, origFullPatch.originalBytes);
    EXPECT_EQ(loadedFullPatch.disassembly, origFullPatch.disassembly);
    EXPECT_EQ(loadedFullPatch.riskLevel, origFullPatch.riskLevel);
    EXPECT_EQ(loadedFullPatch.description, origFullPatch.description);
    EXPECT_EQ(loadedFullPatch.ghidraSymbol, origFullPatch.ghidraSymbol);
    EXPECT_EQ(loadedFullPatch.ghidraNote, origFullPatch.ghidraNote);
    EXPECT_EQ(loadedFullPatch.signatureScore, origFullPatch.signatureScore);
    EXPECT_EQ(loadedFullPatch.signatureLevel, origFullPatch.signatureLevel);
    EXPECT_EQ(loadedFullPatch.signatureFixedBytes, origFullPatch.signatureFixedBytes);
    EXPECT_EQ(loadedFullPatch.signatureWildcardBytes, origFullPatch.signatureWildcardBytes);
    EXPECT_EQ(loadedFullPatch.signatureUniqueFixedBytes, origFullPatch.signatureUniqueFixedBytes);
    EXPECT_DOUBLE_EQ(loadedFullPatch.signatureFixedRatio, origFullPatch.signatureFixedRatio);
    EXPECT_EQ(loadedFullPatch.trainerSafe, origFullPatch.trainerSafe);
    EXPECT_EQ(loadedFullPatch.signatureMatches, origFullPatch.signatureMatches);

    ASSERT_EQ(loaded.autoAsmScripts.size(), original.autoAsmScripts.size());
    EXPECT_EQ(loaded.autoAsmScripts[0].name, original.autoAsmScripts[0].name);
    EXPECT_EQ(loaded.autoAsmScripts[0].scriptText, original.autoAsmScripts[0].scriptText);
    EXPECT_EQ(loaded.autoAsmScripts[0].description, original.autoAsmScripts[0].description);
    EXPECT_EQ(loaded.autoAsmScripts[0].riskLevel, original.autoAsmScripts[0].riskLevel);

    ASSERT_EQ(loaded.luaScripts.size(), original.luaScripts.size());
    EXPECT_EQ(loaded.luaScripts[0].name, original.luaScripts[0].name);
    EXPECT_EQ(loaded.luaScripts[0].scriptText, original.luaScripts[0].scriptText);
    EXPECT_EQ(loaded.luaScripts[0].description, original.luaScripts[0].description);
    EXPECT_EQ(loaded.luaScripts[0].savedAtEpochMs, original.luaScripts[0].savedAtEpochMs);
}

TEST(ProfileStore, MinimalPatchRoundTripsWithZeroedSignatureQuality) {
    // patchToJson() n'ecrit "signatureQuality" que si score>0 ou level non
    // vide (voir profile_store.cpp) -- verifie que patchFromJson() ne plante
    // pas et retombe sur des zeros/vide propres quand la cle est absente.
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString path = dir.filePath("minimal.keprofile");

    Profile original;
    original.gameName = "G";
    original.executableName = "g.exe";
    ProfileCodePatch minimal;
    minimal.name = "Minimal";
    minimal.module = "g.exe";
    minimal.moduleOffset = 0x10;
    minimal.aobPattern = "90";
    minimal.patchBytes = "CC";
    original.patches.append(minimal);

    ASSERT_TRUE(ProfileStore::save(original, path));
    Profile loaded;
    ASSERT_TRUE(ProfileStore::load(path, &loaded));

    ASSERT_EQ(loaded.patches.size(), 1);
    EXPECT_EQ(loaded.patches[0].signatureScore, 0);
    EXPECT_TRUE(loaded.patches[0].signatureLevel.isEmpty());
    EXPECT_FALSE(loaded.patches[0].trainerSafe);
    EXPECT_DOUBLE_EQ(loaded.patches[0].signatureFixedRatio, 0.0);
}

TEST(ProfileStore, LoadsOldProfileMissingNewerFieldsAsEmptyNotCrash) {
    // Simule un .keprofile ecrit avant que patches/autoAsmScripts/luaScripts
    // n'existent (PHASE 19/79/K) : seuls formatVersion/gameName/
    // executableName/targets sont presents. load() doit reussir et rendre
    // des listes vides plutot que d'echouer ou de dereferencer du JSON absent.
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString path = dir.filePath("old.keprofile");

    const QString oldJson = QStringLiteral(
        "{\"formatVersion\":1,\"gameName\":\"Old Game\",\"executableName\":\"old.exe\","
        "\"targets\":[{\"name\":\"Score\",\"type\":\"Int32\",\"locator\":{\"kind\":\"module_offset\","
        "\"module\":\"old.exe\",\"offset\":\"100\",\"lastAddress\":\"0\"}}]}");

    QFile file(path);
    ASSERT_TRUE(file.open(QIODevice::WriteOnly | QIODevice::Text));
    {
        QTextStream stream(&file);
        stream << oldJson;
    }
    file.close();

    Profile loaded;
    ASSERT_TRUE(ProfileStore::load(path, &loaded));
    EXPECT_EQ(loaded.gameName, QStringLiteral("Old Game"));
    ASSERT_EQ(loaded.targets.size(), 1);
    EXPECT_EQ(loaded.targets[0].name, QStringLiteral("Score"));
    EXPECT_EQ(loaded.targets[0].locator.offset, 0x100ULL);
    EXPECT_TRUE(loaded.patches.isEmpty());
    EXPECT_TRUE(loaded.autoAsmScripts.isEmpty());
    EXPECT_TRUE(loaded.luaScripts.isEmpty());
    EXPECT_TRUE(loaded.targets[0].dependsOn.isEmpty());
}

TEST(ProfileStore, ExportPointerMapIncludesOnlyPointerChainsWithMetadata) {
    const Profile profile = buildFullProfile();

    const QJsonObject pointerMap = ProfileStore::exportPointerMap(profile);

    EXPECT_EQ(pointerMap.value("format").toString(), QStringLiteral("killengine.pointer_map"));
    EXPECT_EQ(pointerMap.value("formatVersion").toInt(), 1);
    EXPECT_EQ(pointerMap.value("sourceGameName").toString(), profile.gameName);
    const QJsonArray targets = pointerMap.value("targets").toArray();
    ASSERT_EQ(targets.size(), 1);

    const QJsonObject target = targets[0].toObject();
    EXPECT_EQ(target.value("name").toString(), QStringLiteral("PlayerHealth"));
    EXPECT_EQ(target.value("locator").toObject().value("kind").toString(), QStringLiteral("pointer_chain"));
    const QJsonArray dependencies = target.value("dependsOn").toArray();
    ASSERT_EQ(dependencies.size(), 2);
    EXPECT_EQ(dependencies[0].toString(), QStringLiteral("Money"));
    EXPECT_EQ(dependencies[1].toString(), QStringLiteral("ManagedHealth"));
}

TEST(ProfileStore, MergePointerMapSkipsDuplicatesUnlessReplaceRequested) {
    const Profile source = buildFullProfile();
    const QJsonObject pointerMap = ProfileStore::exportPointerMap(source);

    Profile destination;
    destination.gameName = "Destination";
    destination.executableName = "dest.exe";

    auto firstImport = ProfileStore::mergePointerMap(&destination, pointerMap, false);
    EXPECT_EQ(firstImport.imported, 1);
    EXPECT_EQ(firstImport.replaced, 0);
    EXPECT_EQ(firstImport.skipped, 0);
    ASSERT_EQ(destination.targets.size(), 1);
    EXPECT_EQ(destination.targets[0].name, QStringLiteral("PlayerHealth"));
    EXPECT_EQ(destination.targets[0].dependsOn, QStringList({QStringLiteral("Money"), QStringLiteral("ManagedHealth")}));

    auto duplicateImport = ProfileStore::mergePointerMap(&destination, pointerMap, false);
    EXPECT_EQ(duplicateImport.imported, 0);
    EXPECT_EQ(duplicateImport.skipped, 1);
    ASSERT_EQ(destination.targets.size(), 1);

    QJsonObject replacementMap = pointerMap;
    QJsonArray targets = replacementMap.value("targets").toArray();
    QJsonObject replacementTarget = targets[0].toObject();
    replacementTarget["description"] = "Replacement";
    targets[0] = replacementTarget;
    replacementMap["targets"] = targets;

    auto replacementImport = ProfileStore::mergePointerMap(&destination, replacementMap, true);
    EXPECT_EQ(replacementImport.imported, 1);
    EXPECT_EQ(replacementImport.replaced, 1);
    EXPECT_EQ(replacementImport.skipped, 0);
    ASSERT_EQ(destination.targets.size(), 1);
    EXPECT_EQ(destination.targets[0].description, QStringLiteral("Replacement"));
}

TEST(ProfileStore, LoadFailsCleanlyOnMalformedJson) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString path = dir.filePath("broken.keprofile");

    QFile file(path);
    ASSERT_TRUE(file.open(QIODevice::WriteOnly | QIODevice::Text));
    {
        QTextStream stream(&file);
        stream << "{ this is not valid json";
    }
    file.close();

    Profile loaded;
    EXPECT_FALSE(ProfileStore::load(path, &loaded));
}

TEST(ProfileStore, LoadFailsCleanlyOnMissingFile) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    Profile loaded;
    EXPECT_FALSE(ProfileStore::load(dir.filePath("does-not-exist.keprofile"), &loaded));
}

TEST(ProfileStore, LoadReturnsFalseWithNullOutputPointer) {
    EXPECT_FALSE(ProfileStore::load("irrelevant.keprofile", nullptr));
}

TEST(ProfileStore, ListRemoveRoundTripUsesRealProfilesDirWithUniqueName) {
    // listProfiles()/remove()/profilePath() travaillent sur le dossier reel
    // (%LOCALAPPDATA%\KillEngine\Profiles), contrairement a save/load qui
    // acceptent un chemin complet arbitraire (teste ci-dessus via
    // QTemporaryDir). Nom unique + nettoyage explicite pour ne pas polluer
    // le vrai dossier de profils de la machine qui lance les tests.
    const QString uniqueName = QStringLiteral("killengine_test_profile_store_%1")
        .arg(QDateTime::currentMSecsSinceEpoch());

    ASSERT_TRUE(ProfileStore::ensureProfilesDir());
    Profile profile;
    profile.gameName = "Unique Test Profile";
    profile.executableName = "unique.exe";
    ASSERT_TRUE(ProfileStore::save(profile, ProfileStore::profilePath(uniqueName)));

    EXPECT_TRUE(ProfileStore::listProfiles().contains(uniqueName));

    Profile loaded;
    ASSERT_TRUE(ProfileStore::load(ProfileStore::profilePath(uniqueName), &loaded));
    EXPECT_EQ(loaded.gameName, profile.gameName);

    EXPECT_TRUE(ProfileStore::remove(uniqueName));
    EXPECT_FALSE(ProfileStore::listProfiles().contains(uniqueName));
}
