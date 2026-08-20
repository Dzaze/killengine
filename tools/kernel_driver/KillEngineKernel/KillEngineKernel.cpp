#include <ntddk.h>

namespace {

constexpr ULONG kDeviceType = 0x8000;
constexpr ULONG kIoctlHealthProbe = CTL_CODE(kDeviceType, 0x801, METHOD_BUFFERED, FILE_READ_DATA);
constexpr ULONG kProtocolVersion = 1;
constexpr ULONG kCapabilityHealthProbe = 0x1;

constexpr wchar_t kDeviceName[] = L"\\Device\\KillEngineKernel";
constexpr wchar_t kSymbolicLinkName[] = L"\\DosDevices\\KillEngineKernel";

struct HealthResponse {
    ULONG protocolVersion;
    ULONG flags;
};

void CompleteRequest(_In_ PIRP irp, NTSTATUS status, ULONG_PTR information = 0) {
    irp->IoStatus.Status = status;
    irp->IoStatus.Information = information;
    IoCompleteRequest(irp, IO_NO_INCREMENT);
}

extern "C" DRIVER_UNLOAD DriverUnload;

extern "C" void DriverUnload(_In_ PDRIVER_OBJECT driverObject) {
    UNICODE_STRING symbolicLink;
    RtlInitUnicodeString(&symbolicLink, kSymbolicLinkName);
    IoDeleteSymbolicLink(&symbolicLink);

    if (driverObject->DeviceObject != nullptr) {
        IoDeleteDevice(driverObject->DeviceObject);
    }
}

extern "C" NTSTATUS DispatchCreateClose(_In_ PDEVICE_OBJECT, _Inout_ PIRP irp) {
    CompleteRequest(irp, STATUS_SUCCESS);
    return STATUS_SUCCESS;
}

extern "C" NTSTATUS DispatchDeviceControl(_In_ PDEVICE_OBJECT, _Inout_ PIRP irp) {
    const auto stack = IoGetCurrentIrpStackLocation(irp);
    const auto controlCode = stack->Parameters.DeviceIoControl.IoControlCode;

    if (controlCode != kIoctlHealthProbe) {
        CompleteRequest(irp, STATUS_INVALID_DEVICE_REQUEST);
        return STATUS_INVALID_DEVICE_REQUEST;
    }

    const auto outputLength = stack->Parameters.DeviceIoControl.OutputBufferLength;
    if (outputLength < sizeof(HealthResponse) || irp->AssociatedIrp.SystemBuffer == nullptr) {
        CompleteRequest(irp, STATUS_BUFFER_TOO_SMALL);
        return STATUS_BUFFER_TOO_SMALL;
    }

    auto* response = static_cast<HealthResponse*>(irp->AssociatedIrp.SystemBuffer);
    response->protocolVersion = kProtocolVersion;
    response->flags = kCapabilityHealthProbe;

    CompleteRequest(irp, STATUS_SUCCESS, sizeof(HealthResponse));
    return STATUS_SUCCESS;
}

} // namespace

extern "C" NTSTATUS DriverEntry(_In_ PDRIVER_OBJECT driverObject, _In_ PUNICODE_STRING) {
    UNICODE_STRING deviceName;
    UNICODE_STRING symbolicLink;
    PDEVICE_OBJECT deviceObject = nullptr;

    RtlInitUnicodeString(&deviceName, kDeviceName);
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

    RtlInitUnicodeString(&symbolicLink, kSymbolicLinkName);
    status = IoCreateSymbolicLink(&symbolicLink, &deviceName);
    if (!NT_SUCCESS(status)) {
        IoDeleteDevice(deviceObject);
        return status;
    }

    for (auto& majorFunction : driverObject->MajorFunction) {
        majorFunction = DispatchCreateClose;
    }
    driverObject->MajorFunction[IRP_MJ_CREATE] = DispatchCreateClose;
    driverObject->MajorFunction[IRP_MJ_CLOSE] = DispatchCreateClose;
    driverObject->MajorFunction[IRP_MJ_DEVICE_CONTROL] = DispatchDeviceControl;
    driverObject->DriverUnload = DriverUnload;

    deviceObject->Flags &= ~DO_DEVICE_INITIALIZING;
    return STATUS_SUCCESS;
}
