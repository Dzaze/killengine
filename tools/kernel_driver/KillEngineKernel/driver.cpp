#include "driver.h"
#include <ntifs.h>  // Assure-toi que ce fichier d'en-tête est inclus

namespace {
// Petite zone de sauvegarde des entrees masquee, pour pouvoir les restaurer
// plus tard. Le device serialise les IOCTL, donc pas besoin de verrou.
struct SavedHandleEntry {
    ULONG_PTR index;
    UINT64 low;
    UINT64 high;
    BOOLEAN used;
};
constexpr ULONG kMaxSavedHandleEntries = 256;
SavedHandleEntry g_savedHandleEntries[kMaxSavedHandleEntries] = {};
}

void KillEngineKernelCompleteRequest(_In_ PIRP irp, NTSTATUS status, ULONG_PTR information) {
    irp->IoStatus.Status = status;
    irp->IoStatus.Information = information;
    IoCompleteRequest(irp, IO_NO_INCREMENT);
}

void KillEngineKernelUnload(_In_ PDRIVER_OBJECT driverObject) {
    UNICODE_STRING symbolicLink;
    RtlInitUnicodeString(&symbolicLink, kKillEngineKernelSymbolicLinkName);
    IoDeleteSymbolicLink(&symbolicLink);

    if (driverObject->DeviceObject != nullptr) {
        IoDeleteDevice(driverObject->DeviceObject);
    }
}

NTSTATUS KillEngineKernelDispatchCreateClose(_In_ PDEVICE_OBJECT, _Inout_ PIRP irp) {
    KillEngineKernelCompleteRequest(irp, STATUS_SUCCESS);
    return STATUS_SUCCESS;
}

NTSTATUS KillEngineKernelDispatchDeviceControl(_In_ PDEVICE_OBJECT, _Inout_ PIRP irp) {
    const auto stack = IoGetCurrentIrpStackLocation(irp);
    const auto controlCode = stack->Parameters.DeviceIoControl.IoControlCode;
    const auto inputBuffer = static_cast<PVOID>(irp->AssociatedIrp.SystemBuffer);
    const auto inputLength = stack->Parameters.DeviceIoControl.InputBufferLength;
    const auto outputBuffer = static_cast<PVOID>(irp->AssociatedIrp.SystemBuffer);
    const auto outputLength = stack->Parameters.DeviceIoControl.OutputBufferLength;

    switch (controlCode) {
        case kKillEngineKernelIoctlHealthProbe: {
            if (outputLength < sizeof(KillEngineKernelHealthResponse)) {
                KillEngineKernelCompleteRequest(irp, STATUS_BUFFER_TOO_SMALL);
                return STATUS_BUFFER_TOO_SMALL;
            }

            auto* response = static_cast<KillEngineKernelHealthResponse*>(outputBuffer);
            response->protocolVersion = kKillEngineKernelProtocolVersion;
            response->flags = kKillEngineKernelCapabilityHealthProbe | kKillEngineKernelCapabilityHandleTable;
            response->processMemoryRead = true;
            response->processMemoryWrite = true;

            KillEngineKernelCompleteRequest(irp, STATUS_SUCCESS, sizeof(KillEngineKernelHealthResponse));
            return STATUS_SUCCESS;
        }
        case kKillEngineKernelIoctlReadMemory: {
            if (inputLength < sizeof(KillEngineKernelMemoryRequest)) {
                KillEngineKernelCompleteRequest(irp, STATUS_BUFFER_TOO_SMALL);
                return STATUS_BUFFER_TOO_SMALL;
            }

            auto* request = static_cast<KillEngineKernelMemoryRequest*>(inputBuffer);
            const SIZE_T requestedSize = request->Size;
            if (outputLength < requestedSize) {
                KillEngineKernelCompleteRequest(irp, STATUS_BUFFER_TOO_SMALL);
                return STATUS_BUFFER_TOO_SMALL;
            }

            const PVOID targetAddress = request->Address;
            const HANDLE targetPid = request->ProcessId;

            PEPROCESS process = NULL;
            NTSTATUS status = PsLookupProcessByProcessId(targetPid, &process);
            if (NT_SUCCESS(status)) {
                // Ce WDK (10.0.28000.0) ne declare MmCopyVirtualMemory nulle
                // part (verifie dans ntddk.h/ntifs.h/wdm.h) -- technique
                // standard a la place : s'attacher au contexte d'adressage de
                // la cible (KeStackAttachProcess, deja disponible via
                // ntifs.h, documente depuis Windows 2000) puis copier
                // directement, sous SEH puisqu'une adresse usermode fournie
                // par l'appelant peut toujours devenir invalide entre-temps.
                // Destination = outputBuffer, un buffer NOYAU (SystemBuffer
                // alloue par le gestionnaire d'E/S pour METHOD_BUFFERED) :
                // reste valide quel que soit le contexte de process attache.
                // Cote usermode (kernel_driver_bridge.cpp), readMemory()
                // laisse request->Buffer a nullptr et attend le resultat via
                // le buffer de sortie du DeviceIoControl -- request n'est
                // plus deref apres l'attache (meme SystemBuffer que outputBuffer).
                SIZE_T bytesRead = 0;
                KAPC_STATE apcState;
                KeStackAttachProcess(reinterpret_cast<PRKPROCESS>(process), &apcState);
                __try {
                    ProbeForRead(targetAddress, requestedSize, sizeof(UCHAR));
                    RtlCopyMemory(outputBuffer, targetAddress, requestedSize);
                    bytesRead = requestedSize;
                    status = STATUS_SUCCESS;
                } __except (EXCEPTION_EXECUTE_HANDLER) {
                    status = GetExceptionCode();
                }
                KeUnstackDetachProcess(&apcState);
                ObDereferenceObject(process);
                KillEngineKernelCompleteRequest(irp, status, bytesRead);
            } else {
                KillEngineKernelCompleteRequest(irp, status);
            }
            return status;
        }
        case kKillEngineKernelIoctlWriteMemory: {
            if (inputLength < sizeof(KillEngineKernelMemoryRequest)) {
                KillEngineKernelCompleteRequest(irp, STATUS_BUFFER_TOO_SMALL);
                return STATUS_BUFFER_TOO_SMALL;
            }

            auto* request = static_cast<KillEngineKernelMemoryRequest*>(inputBuffer);
            const SIZE_T requestedSize = request->Size;
            const PVOID targetAddress = request->Address;
            const PVOID sourceBuffer = request->Buffer;  // VA dans le process APPELANT
            const HANDLE targetPid = request->ProcessId;

            // request->Buffer vit dans le process appelant (deja notre
            // contexte courant, l'IRP est traite sur le thread appelant) --
            // impossible d'etre attache aux DEUX process en meme temps pour
            // un memcpy direct source-cible. On passe donc par un buffer
            // noyau intermediaire : copie depuis l'appelant maintenant, puis
            // attache a la cible pour la deuxieme copie.
            PVOID stagingBuffer = ExAllocatePool2(POOL_FLAG_NON_PAGED, requestedSize, 'psEK');
            if (stagingBuffer == nullptr) {
                KillEngineKernelCompleteRequest(irp, STATUS_INSUFFICIENT_RESOURCES);
                return STATUS_INSUFFICIENT_RESOURCES;
            }

            NTSTATUS status;
            __try {
                ProbeForRead(sourceBuffer, requestedSize, sizeof(UCHAR));
                RtlCopyMemory(stagingBuffer, sourceBuffer, requestedSize);
                status = STATUS_SUCCESS;
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                status = GetExceptionCode();
            }

            SIZE_T bytesWritten = 0;
            if (NT_SUCCESS(status)) {
                PEPROCESS process = NULL;
                status = PsLookupProcessByProcessId(targetPid, &process);
                if (NT_SUCCESS(status)) {
                    KAPC_STATE apcState;
                    KeStackAttachProcess(reinterpret_cast<PRKPROCESS>(process), &apcState);
                    __try {
                        ProbeForWrite(targetAddress, requestedSize, sizeof(UCHAR));
                        RtlCopyMemory(targetAddress, stagingBuffer, requestedSize);
                        bytesWritten = requestedSize;
                        status = STATUS_SUCCESS;
                    } __except (EXCEPTION_EXECUTE_HANDLER) {
                        status = GetExceptionCode();
                    }
                    KeUnstackDetachProcess(&apcState);
                    ObDereferenceObject(process);
                }
            }

            ExFreePoolWithTag(stagingBuffer, 'psEK');
            KillEngineKernelCompleteRequest(irp, status, bytesWritten);
            return status;
        }
        case kKillEngineKernelIoctlHandleTable: {
            if (inputLength < sizeof(KillEngineKernelHandleTableRequest)) {
                KillEngineKernelCompleteRequest(irp, STATUS_BUFFER_TOO_SMALL);
                return STATUS_BUFFER_TOO_SMALL;
            }
            if (outputLength < sizeof(KillEngineKernelHandleTableResponse)) {
                KillEngineKernelCompleteRequest(irp, STATUS_BUFFER_TOO_SMALL);
                return STATUS_BUFFER_TOO_SMALL;
            }

            auto* request = static_cast<KillEngineKernelHandleTableRequest*>(inputBuffer);
            auto* response = static_cast<KillEngineKernelHandleTableResponse*>(outputBuffer);
            response->found = FALSE;
            response->entryIndex = 0;
            response->originalObject = 0;

            // owner PID : 0 = process courant (l'appelant IOCTL, i.e. KillEngine)
            HANDLE ownerPid = request->ProcessId;
            if (ownerPid == nullptr) {
                ownerPid = PsGetProcessId(PsGetCurrentProcess());
            }

            PEPROCESS process = nullptr;
            NTSTATUS status = PsLookupProcessByProcessId(ownerPid, &process);
            if (!NT_SUCCESS(status)) {
                KillEngineKernelCompleteRequest(irp, status);
                return status;
            }

            // EPROCESS.ObjectTable — offset documente pour Win10 1903+ / Win11 x64.
            constexpr SIZE_T kOffsetTable = 0x38;
            PVOID table = nullptr;
            __try {
                ProbeForRead(process, sizeof(PVOID), sizeof(PVOID));
                table = *reinterpret_cast<PVOID*>(reinterpret_cast<PCHAR>(process) + kOffsetTable);
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                table = nullptr;
            }
            if (table == nullptr) {
                ObDereferenceObject(process);
                KillEngineKernelCompleteRequest(irp, STATUS_NOT_SUPPORTED);
                return STATUS_NOT_SUPPORTED;
            }

            // table->Size — offset documente. Doit etre une puissance de 2 dans [0x10, 0x10000].
            constexpr SIZE_T kOffsetTableSize = 0x20;
            ULONG tableSize = 0;
            __try {
                ProbeForRead(reinterpret_cast<PCHAR>(table) + kOffsetTableSize, sizeof(ULONG), sizeof(ULONG));
                tableSize = *reinterpret_cast<ULONG*>(reinterpret_cast<PCHAR>(table) + kOffsetTableSize);
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                tableSize = 0;
            }
            if (tableSize < 0x10 || tableSize > 0x10000 || (tableSize & (tableSize - 1)) != 0) {
                ObDereferenceObject(process);
                KillEngineKernelCompleteRequest(irp, STATUS_NOT_SUPPORTED);
                return STATUS_NOT_SUPPORTED;
            }

            // Index de l'entree a partir de la valeur du handle (bits hauts = index).
            const ULONG_PTR rawIndex = (reinterpret_cast<ULONG_PTR>(request->HandleValue) >> 10);
            const ULONG_PTR index = rawIndex & (tableSize - 1);
            response->entryIndex = static_cast<ULONG>(index);

            // Tableau des entrees — offset documente pour Win10 1903+ / Win11 x64
            // (64 champs ULONG reserves apres MaxPagedPool). A ajuster si le layout differe.
            constexpr SIZE_T kOffsetTableEntries = 0x174;
            constexpr SIZE_T kEntrySize = 16;
            PVOID entry = reinterpret_cast<PCHAR>(table) + kOffsetTableEntries + (index * kEntrySize);

            struct Entry { UINT64 low; UINT64 high; };
            auto* entryPtr = reinterpret_cast<Entry*>(entry);
            UINT64 low = 0;
            UINT64 high = 0;
            __try {
                ProbeForRead(entry, kEntrySize, sizeof(UINT64));
                low = entryPtr->low;
                high = entryPtr->high;
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                low = 0;
                high = 0;
            }
            // Garde-fou : un pointeur d'objet valide est aligne sur 8 octets.
            if (low != 0 && (low & 0x7) != 0) {
                low = 0;
                high = 0;
            }
            const BOOLEAN isEmpty = (low == 0 && high == 0);

            if (request->Action == 0) {
                // MASQUER : sauvegarder l'entree puis la mettre a zero pour que
                // l'enumeration (SystemHandleTable) la saute.
                if (!isEmpty) {
                    BOOLEAN saved = FALSE;
                    for (ULONG i = 0; i < kMaxSavedHandleEntries; ++i) {
                        if (!g_savedHandleEntries[i].used) {
                            g_savedHandleEntries[i].index = index;
                            g_savedHandleEntries[i].low = low;
                            g_savedHandleEntries[i].high = high;
                            g_savedHandleEntries[i].used = TRUE;
                            saved = TRUE;
                            break;
                        }
                    }
                    if (!saved) {
                        ObDereferenceObject(process);
                        KillEngineKernelCompleteRequest(irp, STATUS_INSUFFICIENT_RESOURCES);
                        return STATUS_INSUFFICIENT_RESOURCES;
                    }
                    NTSTATUS writeStatus = STATUS_SUCCESS;
                    __try {
                        entryPtr->low = 0;
                        entryPtr->high = 0;
                    } __except (EXCEPTION_EXECUTE_HANDLER) {
                        writeStatus = GetExceptionCode();
                    }
                    if (!NT_SUCCESS(writeStatus)) {
                        for (ULONG i = 0; i < kMaxSavedHandleEntries; ++i) {
                            if (g_savedHandleEntries[i].used && g_savedHandleEntries[i].index == index) {
                                g_savedHandleEntries[i].used = FALSE;
                                break;
                            }
                        }
                        ObDereferenceObject(process);
                        KillEngineKernelCompleteRequest(irp, STATUS_NOT_SUPPORTED);
                        return STATUS_NOT_SUPPORTED;
                    }
                    response->found = TRUE;
                    response->originalObject = low;
                }
            } else {
                // RESTAURER : retrouver l'entree sauvegardee et la reecrire.
                for (ULONG i = 0; i < kMaxSavedHandleEntries; ++i) {
                    if (g_savedHandleEntries[i].used && g_savedHandleEntries[i].index == index) {
                        const UINT64 savedLow = g_savedHandleEntries[i].low;
                        const UINT64 savedHigh = g_savedHandleEntries[i].high;
                        g_savedHandleEntries[i].used = FALSE;
                        NTSTATUS writeStatus = STATUS_SUCCESS;
                        __try {
                            entryPtr->low = savedLow;
                            entryPtr->high = savedHigh;
                        } __except (EXCEPTION_EXECUTE_HANDLER) {
                            writeStatus = GetExceptionCode();
                        }
                        if (!NT_SUCCESS(writeStatus)) {
                            g_savedHandleEntries[i].used = TRUE;
                            ObDereferenceObject(process);
                            KillEngineKernelCompleteRequest(irp, STATUS_NOT_SUPPORTED);
                            return STATUS_NOT_SUPPORTED;
                        }
                        response->found = TRUE;
                        response->originalObject = savedLow;
                        break;
                    }
                }
            }

            ObDereferenceObject(process);
            KillEngineKernelCompleteRequest(irp, STATUS_SUCCESS, sizeof(KillEngineKernelHandleTableResponse));
            return STATUS_SUCCESS;
        }
        default:
            KillEngineKernelCompleteRequest(irp, STATUS_INVALID_DEVICE_REQUEST);
            return STATUS_INVALID_DEVICE_REQUEST;
    }
}

NTSTATUS KillEngineKernelDriverEntry(_In_ PDRIVER_OBJECT driverObject, _In_ PUNICODE_STRING registryPath) {
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
