#include <gtest/gtest.h>

#include "kernel/kernel_driver_bridge.h"

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
}
