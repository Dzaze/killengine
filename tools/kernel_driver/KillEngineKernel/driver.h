#pragma once

#include <ntifs.h>  // Inclure ntifs.h pour les fonctions ZwReadVirtualMemory et ZwWriteVirtualMemory

constexpr ULONG kKillEngineKernelDeviceType = 0x8000;
constexpr ULONG kKillEngineKernelIoctlHealthProbe = CTL_CODE(kKillEngineKernelDeviceType, 0x801, METHOD_BUFFERED, FILE_READ_DATA);
constexpr ULONG kKillEngineKernelIoctlReadMemory = CTL_CODE(kKillEngineKernelDeviceType, 0x802, METHOD_BUFFERED, FILE_READ_DATA);
constexpr ULONG kKillEngineKernelIoctlWriteMemory = CTL_CODE(kKillEngineKernelDeviceType, 0x803, METHOD_BUFFERED, FILE_WRITE_DATA);
constexpr ULONG kKillEngineKernelProtocolVersion = 1;
constexpr ULONG kKillEngineKernelCapabilityHealthProbe = 0x1;

constexpr wchar_t kKillEngineKernelDeviceName[] = L"\\Device\\KillEngineKernel";
constexpr wchar_t kKillEngineKernelSymbolicLinkName[] = L"\\DosDevices\\KillEngineKernel";

struct KillEngineKernelHealthResponse {
    ULONG protocolVersion;
    ULONG flags;
    bool processMemoryRead : 1;
    bool processMemoryWrite : 1;
};

struct KillEngineKernelMemoryRequest {
    HANDLE ProcessId;
    PVOID Address;
    PVOID Buffer;
    SIZE_T Size;
};

void KillEngineKernelCompleteRequest(_In_ PIRP irp, NTSTATUS status, ULONG_PTR information = 0);
void KillEngineKernelUnload(_In_ PDRIVER_OBJECT driverObject);
NTSTATUS KillEngineKernelDispatchCreateClose(_In_ PDEVICE_OBJECT deviceObject, _Inout_ PIRP irp);
NTSTATUS KillEngineKernelDispatchDeviceControl(_In_ PDEVICE_OBJECT deviceObject, _Inout_ PIRP irp);
NTSTATUS KillEngineKernelDriverEntry(_In_ PDRIVER_OBJECT driverObject, _In_ PUNICODE_STRING registryPath);