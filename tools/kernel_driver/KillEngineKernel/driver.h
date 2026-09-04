#pragma once

#include <ntifs.h>  // Inclure ntifs.h pour les fonctions ZwReadVirtualMemory et ZwWriteVirtualMemory

constexpr ULONG kKillEngineKernelDeviceType = 0x8000;
constexpr ULONG kKillEngineKernelIoctlHealthProbe = CTL_CODE(kKillEngineKernelDeviceType, 0x801, METHOD_BUFFERED, FILE_READ_DATA);
constexpr ULONG kKillEngineKernelIoctlReadMemory = CTL_CODE(kKillEngineKernelDeviceType, 0x802, METHOD_BUFFERED, FILE_READ_DATA);
constexpr ULONG kKillEngineKernelIoctlWriteMemory = CTL_CODE(kKillEngineKernelDeviceType, 0x803, METHOD_BUFFERED, FILE_WRITE_DATA);
constexpr ULONG kKillEngineKernelIoctlHandleTable = CTL_CODE(kKillEngineKernelDeviceType, 0x804, METHOD_BUFFERED, FILE_WRITE_DATA);
constexpr ULONG kKillEngineKernelProtocolVersion = 1;
constexpr ULONG kKillEngineKernelCapabilityHealthProbe = 0x1;
constexpr ULONG kKillEngineKernelCapabilityHandleTable = 0x2;

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

// IOCTL 0x804 — hide/restore a specific handle in a process's table (STEALTH-Q3).
// The WDK does not expose HANDLE_TABLE / HANDLE_TABLE_ENTRY, so the handler uses
// minimal, documented offsets for Win10 1903+ / Win11 x64 and fails gracefully
// (STATUS_NOT_SUPPORTED) if the layout does not match.
struct KillEngineKernelHandleTableRequest {
    HANDLE ProcessId;    // owner PID (0 = current process)
    HANDLE HandleValue;  // the specific handle to hide/restore
    ULONG  Action;       // 0 = hide, 1 = restore
};

struct KillEngineKernelHandleTableResponse {
    BOOLEAN found;          // whether the entry existed and was modified
    ULONG   entryIndex;     // index into the table
    UINT64  originalObject; // saved low quad of the entry (diagnostics)
};

void KillEngineKernelCompleteRequest(_In_ PIRP irp, NTSTATUS status, ULONG_PTR information = 0);
void KillEngineKernelUnload(_In_ PDRIVER_OBJECT driverObject);
NTSTATUS KillEngineKernelDispatchCreateClose(_In_ PDEVICE_OBJECT deviceObject, _Inout_ PIRP irp);
NTSTATUS KillEngineKernelDispatchDeviceControl(_In_ PDEVICE_OBJECT deviceObject, _Inout_ PIRP irp);
NTSTATUS KillEngineKernelDriverEntry(_In_ PDRIVER_OBJECT driverObject, _In_ PUNICODE_STRING registryPath);
