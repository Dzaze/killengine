#include "driver.h"

extern "C" NTSTATUS DriverEntry(_In_ PDRIVER_OBJECT driverObject, _In_ PUNICODE_STRING) {
    UNICODE_STRING deviceName;
    UNICODE_STRING symbolicLink;
    PDEVICE_OBJECT deviceObject = nullptr;

    RtlInitUnicodeString(&deviceName, kKillEngineKernelDeviceName);
    NTSTATUS status = IoCreateDevice(driverObject,
                                     0,
                                     &deviceName,
                                     FILE_DEVICE_UNKNOWN,
                                     FILE_DEVICE_SECURE_OPEN,
                                     FALSE,
                                     &deviceObject);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    RtlInitUnicodeString(&symbolicLink, kKillEngineKernelSymbolicLinkName);
    status = IoCreateSymbolicLink(&symbolicLink, &deviceName);
    if (!NT_SUCCESS(status)) {
        IoDeleteDevice(deviceObject);
        return status;
    }

    for (auto& majorFunction : driverObject->MajorFunction) {
        majorFunction = KillEngineKernelDispatchCreateClose;
    }
    driverObject->MajorFunction[IRP_MJ_CREATE] = KillEngineKernelDispatchCreateClose;
    driverObject->MajorFunction[IRP_MJ_CLOSE] = KillEngineKernelDispatchCreateClose;
    driverObject->MajorFunction[IRP_MJ_DEVICE_CONTROL] = KillEngineKernelDispatchDeviceControl;
    driverObject->DriverUnload = KillEngineKernelUnload;

    deviceObject->Flags &= ~DO_DEVICE_INITIALIZING;
    return STATUS_SUCCESS;
}
