#include <gtest/gtest.h>

#include "kernel/kernel_driver_bridge.h"

#ifdef Q_OS_WIN
#include <windows.h>
#endif

using killcore::KernelDriverBridge;
using killcore::KernelDriverProbeStatus;

TEST(KernelDriverBridge, StatusStringsAreStableForUiAndTelemetry) {
    EXPECT_EQ(KernelDriverBridge::statusToString(KernelDriverProbeStatus::Unavailable), QStringLiteral("unavailable"));
    EXPECT_EQ(KernelDriverBridge::statusToString(KernelDriverProbeStatus::Connected), QStringLiteral("connected"));
    EXPECT_EQ(KernelDriverBridge::statusToString(KernelDriverProbeStatus::AccessDenied), QStringLiteral("access_denied"));
    EXPECT_EQ(KernelDriverBridge::statusToString(KernelDriverProbeStatus::Incompatible), QStringLiteral("incompatible"));
    EXPECT_EQ(KernelDriverBridge::statusToString(KernelDriverProbeStatus::Error), QStringLiteral("error"));
}

TEST(KernelDriverBridge, MissingDeviceReportsUnavailableWithoutSideEffects) {
    KernelDriverBridge bridge(QStringLiteral("\\\\.\\KillEngineKernelDefinitelyMissingForTest"));

    const auto result = bridge.probe();

    EXPECT_EQ(result.status, KernelDriverProbeStatus::Unavailable);
    EXPECT_FALSE(result.devicePath.isEmpty());
    EXPECT_FALSE(result.message.isEmpty());
    EXPECT_FALSE(result.capabilities.processMemoryAccess);
    EXPECT_FALSE(result.capabilities.privilegedInstrumentation);
    EXPECT_FALSE(result.capabilities.handleTable);
}

#ifdef Q_OS_WIN
TEST(KernelDriverBridge, HandleTableReturnsFalseWhenDriverMissing) {
    KernelDriverBridge bridge(QStringLiteral("\\\\.\\KillEngineKernelDefinitelyMissingForTest"));

    bool found = true;
    uint64_t entryIndex = 999;
    uint64_t originalObject = 0xDEADBEEF;
    const bool ok = bridge.handleTable(
        reinterpret_cast<HANDLE>(1234),
        0x1234,
        0,
        found,
        entryIndex,
        originalObject);

    EXPECT_FALSE(ok);
    EXPECT_FALSE(found);
    EXPECT_EQ(entryIndex, 0);
    EXPECT_EQ(originalObject, 0);
}
#endif
