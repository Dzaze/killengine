#include "pointer/pointer_chain.h"
#include "profiles/profile_store.h"

#include <QTemporaryDir>
#include <gtest/gtest.h>

using killcore::LocatorKind;
using killcore::PointerChain;
using killcore::Profile;
using killcore::ProfileCodePatch;
using killcore::ProfileStore;
using killcore::ProfileTarget;
using killcore::ValueType;

TEST(PointerChain, EmptyChainIsInvalid) {
    PointerChain chain;
    EXPECT_FALSE(chain.isValid());
}

TEST(PointerChain, ValidChain) {
    PointerChain chain;
    chain.module = "TestModule.exe";
    chain.baseOffset = 0x1000;
    chain.offsets = {0x10, 0x20, 0x30};
    EXPECT_TRUE(chain.isValid());
    EXPECT_EQ(chain.depth(), 3);
}

TEST(PointerChain, ToStringFormat) {
    PointerChain chain;
    chain.module = "game.exe";
    chain.baseOffset = 0xABC;
    chain.offsets = {0x10, 0x20};
    const QString s = chain.toString();
    EXPECT_TRUE(s.contains("game.exe"));
    EXPECT_TRUE(s.contains("abc"));
    EXPECT_TRUE(s.contains("->"));
}

TEST(PointerChain, ModuleRequired) {
    PointerChain chain;
    chain.module = "";  // empty module
    chain.baseOffset = 0x1000;
    chain.offsets = {0x10};
    EXPECT_FALSE(chain.isValid());
}

TEST(PointerChain, OffsetsRequired) {
    PointerChain chain;
    chain.module = "game.exe";
    chain.baseOffset = 0x1000;
    chain.offsets = {};  // no offsets
    EXPECT_FALSE(chain.isValid());
}

TEST(PointerChain, ProfileStoreRoundTripsCodePatches) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());

    Profile profile;
    profile.gameName = "StarCraft2";
    profile.executableName = "SC2_x64.exe";
    // Utilise pour detecter qu'un patch a ete sauvegarde sur une version
    // differente de l'executable (ApplicationController::applyProfileCodePatch) —
    // champ deja present dans le format mais jamais couvert par un test avant.
    profile.executableHash = "deadbeefcafef00d0123456789abcdef0123456789abcdef0123456789abcdef";

    ProfileCodePatch patch;
    patch.name = "Unlimited resources";
    patch.module = "SC2_x64.exe";
    patch.moduleOffset = 0x123456;
    patch.aobPattern = "48 89 05 ?? ?? ?? ??";
    patch.patchBytes = "90 90 90 90 90 90 90";
    patch.originalBytes = "48 89 05 AA BB CC DD";
    patch.disassembly = "mov [rip+0x1234], rax";
    patch.riskLevel = "low";
    patch.description = "Trainer patch test";
    patch.signatureScore = 86;
    patch.signatureLevel = "strong";
    patch.signatureWarning = "Signature AOB robuste.";
    patch.signatureFixedBytes = 12;
    patch.signatureWildcardBytes = 2;
    patch.signatureUniqueFixedBytes = 10;
    patch.signatureFixedRatio = 0.85;
    patch.trainerSafe = true;
    patch.signatureMatches = 1;
    profile.patches.append(patch);

    const QString path = dir.filePath("starcraft2.keprofile");
    ASSERT_TRUE(ProfileStore::save(profile, path));

    Profile loaded;
    ASSERT_TRUE(ProfileStore::load(path, &loaded));
    EXPECT_EQ(loaded.executableHash, profile.executableHash);
    ASSERT_EQ(loaded.patches.size(), 1);
    const auto& loadedPatch = loaded.patches.first();
    EXPECT_EQ(loadedPatch.name, patch.name);
    EXPECT_EQ(loadedPatch.module, patch.module);
    EXPECT_EQ(loadedPatch.moduleOffset, patch.moduleOffset);
    EXPECT_EQ(loadedPatch.aobPattern, patch.aobPattern);
    EXPECT_EQ(loadedPatch.patchBytes, patch.patchBytes);
    EXPECT_EQ(loadedPatch.originalBytes, patch.originalBytes);
    EXPECT_EQ(loadedPatch.disassembly, patch.disassembly);
    EXPECT_EQ(loadedPatch.riskLevel, patch.riskLevel);
    EXPECT_EQ(loadedPatch.description, patch.description);
    EXPECT_EQ(loadedPatch.signatureScore, patch.signatureScore);
    EXPECT_EQ(loadedPatch.signatureLevel, patch.signatureLevel);
    EXPECT_EQ(loadedPatch.signatureWarning, patch.signatureWarning);
    EXPECT_EQ(loadedPatch.signatureFixedBytes, patch.signatureFixedBytes);
    EXPECT_EQ(loadedPatch.signatureWildcardBytes, patch.signatureWildcardBytes);
    EXPECT_EQ(loadedPatch.signatureUniqueFixedBytes, patch.signatureUniqueFixedBytes);
    EXPECT_DOUBLE_EQ(loadedPatch.signatureFixedRatio, patch.signatureFixedRatio);
    EXPECT_EQ(loadedPatch.trainerSafe, patch.trainerSafe);
    EXPECT_EQ(loadedPatch.signatureMatches, patch.signatureMatches);
}

TEST(PointerChain, ProfileStoreRoundTripsAutoAsmScripts) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());

    Profile profile;
    profile.gameName = "StarCraft2";
    profile.executableName = "SC2_x64.exe";

    killcore::ProfileAutoAsmScript script;
    script.name = "Infinite HP";
    script.scriptText = "alloc(newmem,256)\nlabel(returnhere)\n\nnewmem:\nmov [rax+8], 9999\njmp returnhere\n\n\"SC2_x64.exe\"+0x123456:\njmp newmem\nnop\nreturnhere:";
    script.description = "Fige les PV via un trampoline";
    script.riskLevel = "high";
    profile.autoAsmScripts.append(script);

    const QString path = dir.filePath("starcraft2.keprofile");
    ASSERT_TRUE(ProfileStore::save(profile, path));

    Profile loaded;
    ASSERT_TRUE(ProfileStore::load(path, &loaded));
    ASSERT_EQ(loaded.autoAsmScripts.size(), 1);
    const auto& loadedScript = loaded.autoAsmScripts.first();
    EXPECT_EQ(loadedScript.name, script.name);
    EXPECT_EQ(loadedScript.scriptText, script.scriptText);
    EXPECT_EQ(loadedScript.description, script.description);
    EXPECT_EQ(loadedScript.riskLevel, script.riskLevel);
}

TEST(PointerChain, ProfileStoreRoundTripsLuaScripts) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());

    Profile profile;
    profile.gameName = "StarCraft2";
    profile.executableName = "SC2_x64.exe";

    killcore::ProfileLuaScript script;
    script.name = "Ping test";
    script.scriptText = "local ke = require(\"killengine\")\nprint(ke.call(\"ping\", { \"hello\" }))";
    script.description = "Verifie la connexion au pipe d'automatisation";
    script.savedAtEpochMs = 1755000000000LL;
    profile.luaScripts.append(script);

    const QString path = dir.filePath("starcraft2.keprofile");
    ASSERT_TRUE(ProfileStore::save(profile, path));

    Profile loaded;
    ASSERT_TRUE(ProfileStore::load(path, &loaded));
    ASSERT_EQ(loaded.luaScripts.size(), 1);
    const auto& loadedScript = loaded.luaScripts.first();
    EXPECT_EQ(loadedScript.name, script.name);
    EXPECT_EQ(loadedScript.scriptText, script.scriptText);
    EXPECT_EQ(loadedScript.description, script.description);
    EXPECT_EQ(loadedScript.savedAtEpochMs, script.savedAtEpochMs);
}

TEST(PointerChain, ProfileStoreRoundTripsPointerChainLocator) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());

    Profile profile;
    profile.gameName = "StarCraft2";
    profile.executableName = "SC2_x64.exe";

    ProfileTarget target;
    target.name = "Minerals";
    target.type = ValueType::Int32;
    target.locator.kind = LocatorKind::PointerChain;
    target.locator.lastAddress = 0x7ff600001234;
    target.locator.pointerChain.module = "SC2_x64.exe";
    target.locator.pointerChain.baseOffset = 0x123456;
    target.locator.pointerChain.offsets = {0x50, 0x10, 0x20};
    target.description = "Pointer chain test";
    profile.targets.append(target);

    const QString path = dir.filePath("starcraft2.keprofile");
    ASSERT_TRUE(ProfileStore::save(profile, path));

    Profile loaded;
    ASSERT_TRUE(ProfileStore::load(path, &loaded));
    ASSERT_EQ(loaded.targets.size(), 1);
    const auto& loadedTarget = loaded.targets.first();
    EXPECT_EQ(loadedTarget.locator.kind, LocatorKind::PointerChain);
    EXPECT_EQ(loadedTarget.locator.pointerChain.module, "SC2_x64.exe");
    EXPECT_EQ(loadedTarget.locator.pointerChain.baseOffset, 0x123456);
    ASSERT_EQ(loadedTarget.locator.pointerChain.offsets.size(), 3);
    EXPECT_EQ(loadedTarget.locator.pointerChain.offsets[0], 0x50);
    EXPECT_EQ(loadedTarget.locator.pointerChain.offsets[1], 0x10);
    EXPECT_EQ(loadedTarget.locator.pointerChain.offsets[2], 0x20);
    EXPECT_EQ(loadedTarget.locator.lastAddress, 0x7ff600001234);
}

TEST(PointerChain, ProfileStoreRoundTripsClrFieldLocator) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());

    Profile profile;
    profile.gameName = "ManagedGame";
    profile.executableName = "ManagedGame.exe";

    ProfileTarget target;
    target.name = "PlayerHealth";
    target.type = ValueType::Int32;
    target.locator.kind = LocatorKind::ClrField;
    target.locator.lastAddress = 0x2476e00acd8;
    target.locator.clrField.typeSubstring = "Game.Player";
    target.locator.clrField.identityField = "Name";
    target.locator.clrField.identityValue = "MainPlayer";
    target.locator.clrField.targetField = "Health";
    target.description = "CLR field locator test";
    profile.targets.append(target);

    const QString path = dir.filePath("managed.keprofile");
    ASSERT_TRUE(ProfileStore::save(profile, path));

    Profile loaded;
    ASSERT_TRUE(ProfileStore::load(path, &loaded));
    ASSERT_EQ(loaded.targets.size(), 1);
    const auto& loadedTarget = loaded.targets.first();
    EXPECT_EQ(loadedTarget.locator.kind, LocatorKind::ClrField);
    EXPECT_EQ(loadedTarget.locator.lastAddress, 0x2476e00acd8);
    EXPECT_EQ(loadedTarget.locator.clrField.typeSubstring, "Game.Player");
    EXPECT_EQ(loadedTarget.locator.clrField.identityField, "Name");
    EXPECT_EQ(loadedTarget.locator.clrField.identityValue, "MainPlayer");
    EXPECT_EQ(loadedTarget.locator.clrField.targetField, "Health");
    EXPECT_TRUE(loadedTarget.locator.isValid());
    EXPECT_TRUE(loadedTarget.locator.toString().contains("CLR"));
}
