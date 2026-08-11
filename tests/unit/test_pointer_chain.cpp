#include "pointer/pointer_chain.h"
#include "profiles/profile_store.h"

#include <QTemporaryDir>
#include <gtest/gtest.h>

using killcore::LocatorKind;
using killcore::PointerChain;
using killcore::Profile;
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
