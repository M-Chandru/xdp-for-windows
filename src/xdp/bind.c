//
// Copyright (C) Microsoft Corporation. All rights reserved.
//

//
// This module provides:
//
// 1. A single abstraction for core XDP modules to manipulate disparate XDP
//    interface types. This module implements the core XDP side of the XDP IF
//    API.
// 2. A single work queue for each interface, since the external-facing XDP
//    control path is serialized. Core XDP components can schedule their own
//    work on this queue, reducing the need for locking schemes across
//    components.
//

#include "precomp.h"
#include "bind.tmh"

typedef struct _XDP_INTERFACE_SET XDP_INTERFACE_SET;
typedef struct _XDP_INTERFACE_NMR XDP_INTERFACE_NMR;

typedef struct _XDP_INTERFACE {
    NET_IFINDEX IfIndex;
    XDP_INTERFACE_SET *IfSet;
    VOID *XdpIfInterfaceContext;

    CONST XDP_CAPABILITIES_INTERNAL Capabilities;
    XDP_REMOVE_INTERFACE_COMPLETE *RemoveInterfaceComplete;
    XDP_INTERFACE_CONFIG_DETAILS OpenConfig;

    XDP_INTERFACE_NMR *Nmr;
    XDP_VERSION DriverApiVersion;
    CONST XDP_INTERFACE_DISPATCH *InterfaceDispatch;
    VOID *InterfaceContext;

    LONG ReferenceCount;

    LIST_ENTRY Clients;         // Components bound to the NIC.
    ULONG ProviderReference;    // Active reference on the NIC.

    union {
        struct {
            BOOLEAN BindingDeleting : 1;    // The interface is being deleted.
            BOOLEAN NmrDeleting : 1;        // The NMR binding is being deleted.
        };
        BOOLEAN Rundown;            // Disable new active references on the NIC.
    };

    XDP_WORK_QUEUE *WorkQueue;
    union {
        XDP_BINDING_WORKITEM CloseWorkItem;    // Guaranteed item for close.
        XDP_BINDING_WORKITEM DeleteWorkItem;   // Guaranteed item for delete.
    };
} XDP_INTERFACE;

typedef struct _XDP_INTERFACE_NMR {
    //
    // To support NMR teardown by both XDP interface and XDP platform, this NMR
    // context lasts until the NMR binding is cleaned up (on the worker thread)
    // and the NMR detach notification work item executes (also on the worker
    // thread). The workers may execute in either order; after both have
    // executed, this NMR context is freed.
    //
    XDP_PROVIDER_HANDLE NmrHandle;
    KEVENT DetachNotification;
    UINT32 ReferenceCount;
    XDP_BINDING_WORKITEM WorkItem;
} XDP_INTERFACE_NMR;

typedef struct _XDP_INTERFACE_SET {
    LIST_ENTRY Link;
    NET_IFINDEX IfIndex;
    VOID *XdpIfInterfaceSetContext;
    XDP_INTERFACE *Interfaces[2];   // One binding for both generic and native.
} XDP_INTERFACE_SET;

//
// Latest version of the XDP driver API.
//
static CONST XDP_VERSION XdpDriverApiCurrentVersion = {
    .Major = XDP_DRIVER_API_MAJOR_VER,
    .Minor = XDP_DRIVER_API_MINOR_VER,
    .Patch = XDP_DRIVER_API_PATCH_VER
};

static EX_PUSH_LOCK XdpInterfaceSetsLock;
static LIST_ENTRY XdpInterfaceSets;
static BOOLEAN XdpBindInitialized = FALSE;

static
XDP_INTERFACE *
XdpInterfaceFromConfig(
    _In_ XDP_INTERFACE_CONFIG InterfaceConfig
    )
{
    return CONTAINING_RECORD(InterfaceConfig, XDP_INTERFACE, OpenConfig);
}

static
BOOLEAN
XdpValidateCapabilitiesEx(
    _In_ CONST XDP_CAPABILITIES_EX *CapabilitiesEx,
    _In_ UINT32 TotalSize
    )
{
    UINT32 Size;
    NTSTATUS Status;

    if (CapabilitiesEx->Header.Revision < XDP_CAPABILITIES_EX_REVISION_1 ||
        CapabilitiesEx->Header.Size < XDP_SIZEOF_CAPABILITIES_EX_REVISION_1) {
        return FALSE;
    }

    Status =
        RtlUInt32Mult(
            CapabilitiesEx->DriverApiVersionCount, sizeof(XDP_VERSION), &Size);
    if (!NT_SUCCESS(Status)) {
        return FALSE;
    }

    Status =
        RtlUInt32Add(
            CapabilitiesEx->DriverApiVersionsOffset, Size, &Size);
    if (!NT_SUCCESS(Status)) {
        return FALSE;
    }

    return TotalSize >= Size;
}

CONST XDP_VERSION *
XdpGetDriverApiVersion(
    _In_ XDP_INTERFACE_CONFIG InterfaceConfig
    )
{
    XDP_INTERFACE *Interface = XdpInterfaceFromConfig(InterfaceConfig);
    return &Interface->DriverApiVersion;
}

static CONST XDP_INTERFACE_CONFIG_DISPATCH XdpOpenDispatch = {
    .Header         = {
        .Revision   = XDP_INTERFACE_CONFIG_DISPATCH_REVISION_1,
        .Size       = XDP_SIZEOF_INTERFACE_CONFIG_DISPATCH_REVISION_1
    },
    .GetDriverApiVersion = XdpGetDriverApiVersion
};

static
VOID
XdpIfpInterfaceNmrDelete(
    _In_ XDP_BINDING_WORKITEM *Item
    );

static
VOID
XdpIfpReferenceInterface(
    _Inout_ XDP_INTERFACE *Interface
    )
{
    InterlockedIncrement(&Interface->ReferenceCount);
}

static
VOID
XdpIfpDereferenceInterface(
    _Inout_ XDP_INTERFACE *Interface
    )
{
    if (InterlockedDecrement(&Interface->ReferenceCount) == 0) {
        ASSERT(Interface->ProviderReference == 0);
        if (Interface->WorkQueue != NULL) {
            XdpShutdownWorkQueue(Interface->WorkQueue, FALSE);
        }
        ExFreePoolWithTag(Interface, XDP_POOLTAG_IF);
    }
}

VOID
XdpIfDereferenceBinding(
    _In_ XDP_BINDING_HANDLE BindingHandle
    )
{
    XdpIfpDereferenceInterface((XDP_INTERFACE *)BindingHandle);
}

static
VOID
XdpIfpDereferenceNmr(
    _In_ XDP_INTERFACE_NMR *Nmr
    )
{
    if (--Nmr->ReferenceCount == 0) {
        ASSERT(Nmr->NmrHandle == NULL);
        ExFreePoolWithTag(Nmr, XDP_POOLTAG_BINDING);
    }
}

static
VOID
XdpIfpDetachNmrInterface(
    _In_ VOID *ProviderContext
    )
{
    XDP_INTERFACE_NMR *Nmr = ProviderContext;
    XDP_INTERFACE *Interface = (XDP_INTERFACE *)Nmr->WorkItem.BindingHandle;

    TraceVerbose(
        TRACE_CORE, "IfIndex=%u Mode=%!XDP_MODE! NMR detach notification",
        Interface->IfIndex, Interface->Capabilities.Mode);

    KeSetEvent(&Nmr->DetachNotification, 0, FALSE);
    XdpIfQueueWorkItem(&Nmr->WorkItem);
    XdpIfpDereferenceInterface(Interface);
}

static
VOID
XdpIfpCloseNmrInterface(
    _In_ XDP_INTERFACE *Interface
    )
{
    XDP_INTERFACE_NMR *Nmr = Interface->Nmr;

    TraceEnter(
        TRACE_CORE, "IfIndex=%u Mode=%!XDP_MODE!",
        Interface->IfIndex, Interface->Capabilities.Mode);

    ASSERT(Interface->ProviderReference == 0);
    ASSERT(Interface->InterfaceContext == NULL);
    ASSERT(Nmr != NULL && Nmr->NmrHandle != NULL);

    XdpCloseProvider(Nmr->NmrHandle);
    KeWaitForSingleObject(&Nmr->DetachNotification, Executive, KernelMode, FALSE, NULL);
    XdpCleanupProvider(Nmr->NmrHandle);
    Nmr->NmrHandle = NULL;

    Interface->Nmr = NULL;
    XdpIfpDereferenceNmr(Nmr);

    TraceExit(TRACE_CORE);
}

static
VOID
XdpIfpInvokeCloseInterface(
    _In_ XDP_INTERFACE *Interface
    )
{
    TraceEnter(
        TRACE_CORE, "IfIndex=%u Mode=%!XDP_MODE!",
        Interface->IfIndex, Interface->Capabilities.Mode);

    if (Interface->InterfaceDispatch->CloseInterface != NULL) {
        Interface->InterfaceDispatch->CloseInterface(Interface->InterfaceContext);
    }

    TraceExit(TRACE_CORE);
}

static
VOID
XdpIfpCloseInterface(
    _In_ XDP_INTERFACE *Interface
    )
{
    TraceEnter(
        TRACE_CORE, "IfIndex=%u Mode=%!XDP_MODE!",
        Interface->IfIndex, Interface->Capabilities.Mode);

    if (Interface->InterfaceContext != NULL) {
        XdpIfpInvokeCloseInterface(Interface);
        Interface->InterfaceDispatch = NULL;
        Interface->InterfaceContext = NULL;
    }

    if (Interface->Nmr != NULL) {
        XdpIfpCloseNmrInterface(Interface);
        Interface->NmrDeleting = FALSE;

        TraceVerbose(TRACE_CORE, "interface closed");
    }

    if (Interface->BindingDeleting && Interface->XdpIfInterfaceContext != NULL) {
        Interface->RemoveInterfaceComplete(Interface->XdpIfInterfaceContext);
        Interface->XdpIfInterfaceContext = NULL;

        TraceVerbose(TRACE_CORE, "interface deregistration completed");
    }

    TraceExit(TRACE_CORE);
}

static
NTSTATUS
XdpIfpInvokeOpenInterface(
    _In_ XDP_INTERFACE *Interface,
    _In_opt_ VOID *InterfaceContext,
    _In_ CONST XDP_INTERFACE_DISPATCH *InterfaceDispatch
    )
{
    NTSTATUS Status = STATUS_SUCCESS;

    TraceEnter(
        TRACE_CORE, "IfIndex=%u Mode=%!XDP_MODE!",
        Interface->IfIndex, Interface->Capabilities.Mode);

    if (InterfaceDispatch->OpenInterface != NULL) {
        ASSERT(InterfaceContext);
        Status =
            InterfaceDispatch->OpenInterface(
                InterfaceContext, (XDP_INTERFACE_CONFIG)&Interface->OpenConfig);
    }

    TraceExitStatus(TRACE_CORE);

    return Status;
}

static
BOOLEAN
XdpVersionIsSupported(
    _In_ CONST XDP_VERSION *Version,
    _In_ CONST XDP_VERSION *MinimumSupportedVersion
    )
{
    return
        Version->Major == MinimumSupportedVersion->Major &&
        Version->Minor >= MinimumSupportedVersion->Minor &&
        Version->Patch >= MinimumSupportedVersion->Patch;
}

static
_IRQL_requires_(PASSIVE_LEVEL)
NTSTATUS
XdpRequestClientDispatch(
    _In_ CONST XDP_CAPABILITIES_EX *ClientCapabilitiesEx,
    _In_ VOID *GetInterfaceContext,
    _In_ XDP_GET_INTERFACE_DISPATCH  *GetInterfaceDispatch,
    _Inout_ XDP_INTERFACE *Interface,
    _Out_ VOID **InterfaceContext,
    _Out_ CONST XDP_INTERFACE_DISPATCH  **InterfaceDispatch
    )
{
    NTSTATUS Status = STATUS_NOT_SUPPORTED;
    XDP_VERSION *DriverApiVersions =
        RTL_PTR_ADD(
            ClientCapabilitiesEx, ClientCapabilitiesEx->DriverApiVersionsOffset);

    for (UINT32 i = 0; i < ClientCapabilitiesEx->DriverApiVersionCount; i++) {
        CONST XDP_VERSION *ClientVersion = &DriverApiVersions[i];

        if (XdpVersionIsSupported(&XdpDriverApiCurrentVersion, ClientVersion)) {
            Status =
                GetInterfaceDispatch(
                    ClientVersion, GetInterfaceContext,
                    InterfaceContext, InterfaceDispatch);
            if (NT_SUCCESS(Status)) {
                Interface->DriverApiVersion = *ClientVersion;
                TraceInfo(
                    TRACE_CORE, "IfIndex=%u Mode=%!XDP_MODE! Received interface dispatch"
                    " table for ClientVersion=%u.%u.%u",
                    Interface->IfIndex, Interface->Capabilities.Mode,
                    ClientVersion->Major, ClientVersion->Minor, ClientVersion->Patch);
                break;
            } else {
                TraceWarn(
                    TRACE_CORE,
                    "IfIndex=%u Mode=%!XDP_MODE! Failed to get interface dispatch table"
                    " Status=%!STATUS!", Interface->IfIndex, Interface->Capabilities.Mode,
                    Status);
                Status = STATUS_NOT_SUPPORTED;
            }
        }
    }

    if (!NT_SUCCESS(Status)) {
        TraceWarn(
            TRACE_CORE, "IfIndex=%u Mode=%!XDP_MODE! No compatible interface was found",
            Interface->IfIndex, Interface->Capabilities.Mode);
    }
    return Status;
}

static
_IRQL_requires_(PASSIVE_LEVEL)
NTSTATUS
XdpIfpOpenInterface(
    _Inout_ XDP_INTERFACE *Interface
    )
{
    CONST XDP_CAPABILITIES_EX *CapabilitiesEx = Interface->Capabilities.CapabilitiesEx;
    VOID *GetInterfaceContext;
    XDP_GET_INTERFACE_DISPATCH  *GetInterfaceDispatch;
    VOID *InterfaceContext;
    CONST XDP_INTERFACE_DISPATCH *InterfaceDispatch;
    NTSTATUS Status;

    TraceEnter(
        TRACE_CORE, "IfIndex=%u Mode=%!XDP_MODE!",
        Interface->IfIndex, Interface->Capabilities.Mode);

    if (CapabilitiesEx->Header.Revision < XDP_CAPABILITIES_EX_REVISION_1 ||
        CapabilitiesEx->Header.Size < XDP_SIZEOF_CAPABILITIES_EX_REVISION_1) {
        TraceError(
            TRACE_CORE, "IfIndex=%u Mode=%!XDP_MODE! Invalid capabilities",
            Interface->IfIndex, Interface->Capabilities.Mode);
        Status = STATUS_NOT_SUPPORTED;
        goto Exit;
    }

    ASSERT(Interface->Nmr == NULL);

    Interface->Nmr =
        ExAllocatePoolZero(NonPagedPoolNx, sizeof(*Interface->Nmr), XDP_POOLTAG_BINDING);
    if (Interface->Nmr == NULL) {
        TraceError(
            TRACE_CORE, "IfIndex=%u Mode=%!XDP_MODE! NMR allocation failed",
            Interface->IfIndex, Interface->Capabilities.Mode);
        Status = STATUS_NO_MEMORY;
        goto Exit;
    }

    if (!XdpIsFeOrLater() && Interface->Capabilities.Mode == XDP_INTERFACE_MODE_NATIVE) {
        TraceWarn(TRACE_CORE, "Opening a native XDP interface on an unsupported OS");
    }

    XdpIfpReferenceInterface(Interface);
    Interface->Nmr->ReferenceCount = 2;
    Interface->Nmr->WorkItem.BindingHandle = (XDP_BINDING_HANDLE)Interface;
    Interface->Nmr->WorkItem.WorkRoutine = XdpIfpInterfaceNmrDelete;
    KeInitializeEvent(&Interface->Nmr->DetachNotification, NotificationEvent, FALSE);

    Status =
        XdpOpenProvider(
            Interface->IfIndex, &CapabilitiesEx->InstanceId, Interface->Nmr,
            XdpIfpDetachNmrInterface, &GetInterfaceContext, &GetInterfaceDispatch,
            &Interface->Nmr->NmrHandle);
    if (!NT_SUCCESS(Status)) {
        TraceError(
            TRACE_CORE, "IfIndex=%u Mode=%!XDP_MODE! Failed to open NMR binding",
            Interface->IfIndex, Interface->Capabilities.Mode);
        goto Exit;
    }

    Status =
        XdpRequestClientDispatch(
            CapabilitiesEx, GetInterfaceContext, GetInterfaceDispatch,
            Interface, &InterfaceContext, &InterfaceDispatch);
    if (!NT_SUCCESS(Status)) {
        goto Exit;
    }

    Interface->InterfaceContext = InterfaceContext;
    Interface->InterfaceDispatch = InterfaceDispatch;

    Status = XdpIfpInvokeOpenInterface(Interface, InterfaceContext, InterfaceDispatch);
    if (!NT_SUCCESS(Status)) {
        TraceError(
            TRACE_CORE, "IfIndex=%u Mode=%!XDP_MODE! Interface open failed",
            Interface->IfIndex, Interface->Capabilities.Mode);
        goto Exit;
    }

    Status = STATUS_SUCCESS;

Exit:

    if (!NT_SUCCESS(Status)) {
        if (Interface->Nmr != NULL) {
            if (Interface->Nmr->NmrHandle != NULL) {
                XdpIfpCloseInterface(Interface);
            } else {
                XdpIfpDereferenceInterface(Interface);
                ExFreePoolWithTag(Interface->Nmr, XDP_POOLTAG_BINDING);
            }
            Interface->Nmr = NULL;
        }
    }

    TraceExitStatus(TRACE_CORE);

    return Status;
}

static
_IRQL_requires_(PASSIVE_LEVEL)
_Requires_lock_held_(&XdpInterfaceSetsLock)
XDP_INTERFACE_SET *
XdpIfpFindIfSet(
    _In_ NET_IFINDEX IfIndex
    )
{
    XDP_INTERFACE_SET *IfSet = NULL;
    LIST_ENTRY *Entry = XdpInterfaceSets.Flink;

    while (Entry != &XdpInterfaceSets) {
        XDP_INTERFACE_SET *Candidate = CONTAINING_RECORD(Entry, XDP_INTERFACE_SET, Link);
        Entry = Entry->Flink;

        if (Candidate->IfIndex == IfIndex) {
            IfSet = Candidate;
            break;
        }
    }

    return IfSet;
}

_IRQL_requires_(PASSIVE_LEVEL)
BOOLEAN
XdpIfSupportsHookId(
    _In_ CONST XDP_CAPABILITIES_INTERNAL *Capabilities,
    _In_ CONST XDP_HOOK_ID *Target
    )
{
    for (UINT32 Index = 0; Index < Capabilities->HookCount; Index++) {
        CONST XDP_HOOK_ID *Candidate = &Capabilities->Hooks[Index];

        if (Target->Layer == Candidate->Layer &&
            Target->Direction == Candidate->Direction &&
            Target->SubLayer == Candidate->SubLayer) {
            return TRUE;
        }
    }

    return FALSE;
}

static
BOOLEAN
XdpIfpSupportsHookIds(
    _In_ CONST XDP_CAPABILITIES_INTERNAL *Capabilities,
    _In_ CONST XDP_HOOK_ID *TargetIds,
    _In_ UINT32 TargetCount
    )
{
    for (UINT32 TargetIndex = 0; TargetIndex < TargetCount; TargetIndex++) {
        if (!XdpIfSupportsHookId(Capabilities, &TargetIds[TargetIndex])) {
            return FALSE;
        }
    }

    return TRUE;
}

static
_IRQL_requires_(PASSIVE_LEVEL)
_Requires_lock_held_(&XdpInterfaceSetsLock)
XDP_INTERFACE *
XdpIfpFindInterface(
    _In_ NET_IFINDEX IfIndex,
    _In_ CONST XDP_HOOK_ID *HookIds,
    _In_ UINT32 HookCount,
    _In_opt_ XDP_INTERFACE_MODE *RequiredMode
    )
{
    XDP_INTERFACE_SET *IfSet = NULL;
    XDP_INTERFACE *Interface = NULL;

    IfSet = XdpIfpFindIfSet(IfIndex);
    if (IfSet == NULL) {
        goto Exit;
    }

    //
    // Find the best interface matching the caller constraints.
    //
    for (XDP_INTERFACE_MODE Mode = XDP_INTERFACE_MODE_GENERIC;
        Mode <= XDP_INTERFACE_MODE_NATIVE;
        Mode++) {
        XDP_INTERFACE *CandidateIf = IfSet->Interfaces[Mode];

        if (CandidateIf == NULL) {
            continue;
        }

        if (RequiredMode != NULL && *RequiredMode != Mode) {
            continue;
        }

        if (!XdpIfpSupportsHookIds(&CandidateIf->Capabilities, HookIds, HookCount)) {
            continue;
        }

        Interface = CandidateIf;
    }

Exit:

    return Interface;
}

_IRQL_requires_(PASSIVE_LEVEL)
XDP_BINDING_HANDLE
XdpIfFindAndReferenceBinding(
    _In_ NET_IFINDEX IfIndex,
    _In_ CONST XDP_HOOK_ID *HookIds,
    _In_ UINT32 HookCount,
    _In_opt_ XDP_INTERFACE_MODE *RequiredMode
    )
{
    XDP_INTERFACE *Interface = NULL;

    ExAcquirePushLockShared(&XdpInterfaceSetsLock);
    Interface = XdpIfpFindInterface(IfIndex, HookIds, HookCount, RequiredMode);
    if (Interface != NULL) {
        XdpIfpReferenceInterface(Interface);
    }
    ExReleasePushLockShared(&XdpInterfaceSetsLock);

    return (XDP_BINDING_HANDLE)Interface;
}

VOID
XdpIfQueueWorkItem(
    _In_ XDP_BINDING_WORKITEM *WorkItem
    )
{
    XDP_INTERFACE *Interface = (XDP_INTERFACE *)WorkItem->BindingHandle;

    WorkItem->IdealNode = KeGetCurrentNodeNumber();
    XdpIfpReferenceInterface(Interface);
    XdpInsertWorkQueue(Interface->WorkQueue, &WorkItem->Link);
}

CONST XDP_CAPABILITIES_INTERNAL *
XdpIfGetCapabilities(
    _In_ XDP_BINDING_HANDLE BindingHandle
    )
{
    XDP_INTERFACE *Interface = (XDP_INTERFACE *)BindingHandle;

    return &Interface->Capabilities;
}

static
VOID
XdpIfpStartRundown(
    _In_ XDP_INTERFACE *Interface
    )
{
    TraceEnter(
        TRACE_CORE, "IfIndex=%u Mode=%!XDP_MODE!",
        Interface->IfIndex, Interface->Capabilities.Mode);

    if (Interface->ProviderReference == 0) {
        XdpIfpCloseInterface(Interface);
    }

    while (!IsListEmpty(&Interface->Clients)) {
        XDP_BINDING_CLIENT_ENTRY *ClientEntry =
            CONTAINING_RECORD(Interface->Clients.Flink, XDP_BINDING_CLIENT_ENTRY, Link);

        RemoveEntryList(&ClientEntry->Link);
        InitializeListHead(&ClientEntry->Link);

        ClientEntry->Client->BindingDetached(ClientEntry);

        XdpIfpDereferenceInterface(Interface);
    }

    TraceExit(TRACE_CORE);
}

static
VOID
XdpIfpInterfaceDelete(
    _In_ XDP_BINDING_WORKITEM *Item
    )
{
    XDP_INTERFACE *Interface = (XDP_INTERFACE *)Item->BindingHandle;

    TraceEnter(
        TRACE_CORE, "IfIndex=%u Mode=%!XDP_MODE!",
        Interface->IfIndex, Interface->Capabilities.Mode);

    Interface->BindingDeleting = TRUE;

    XdpIfpStartRundown(Interface);

    //
    // Release the initial binding reference.
    //
    XdpIfpDereferenceInterface(Interface);

    TraceExit(TRACE_CORE);
}

static
VOID
XdpIfpInterfaceNmrDelete(
    _In_ XDP_BINDING_WORKITEM *Item
    )
{
    XDP_INTERFACE *Interface = (XDP_INTERFACE *)Item->BindingHandle;
    XDP_INTERFACE_NMR *Nmr = CONTAINING_RECORD(Item, XDP_INTERFACE_NMR, WorkItem);

    TraceEnter(
        TRACE_CORE, "IfIndex=%u Mode=%!XDP_MODE!",
        Interface->IfIndex, Interface->Capabilities.Mode);

    if (Nmr->NmrHandle != NULL) {
        ASSERT(Interface->NmrDeleting == FALSE);
        Interface->NmrDeleting = TRUE;

        XdpIfpStartRundown(Interface);
    }

    XdpIfpDereferenceNmr(Nmr);

    TraceExit(TRACE_CORE);
}

static
_IRQL_requires_(PASSIVE_LEVEL)
VOID
XdpIfpInterfaceWorker(
    _In_ SINGLE_LIST_ENTRY *WorkQueueHead
    )
{
    while (WorkQueueHead != NULL) {
        XDP_BINDING_WORKITEM *Item;
        XDP_INTERFACE *Interface;
        GROUP_AFFINITY Affinity;
        GROUP_AFFINITY OldAffinity;

        Item = CONTAINING_RECORD(WorkQueueHead, XDP_BINDING_WORKITEM, Link);
        Interface = (XDP_INTERFACE *)Item->BindingHandle;
        WorkQueueHead = WorkQueueHead->Next;

        //
        // Perform work on the original caller's NUMA node. Note that WS2022
        // introduces a multi-affinity-group NUMA concept not implemented here.
        //
        KeQueryNodeActiveAffinity(Item->IdealNode, &Affinity, NULL);
        KeSetSystemGroupAffinityThread(&Affinity, &OldAffinity);

        Item->WorkRoutine(Item);

        KeRevertToUserGroupAffinityThread(&OldAffinity);

        XdpIfpDereferenceInterface(Interface);
    }
}

_IRQL_requires_max_(PASSIVE_LEVEL)
NTSTATUS
XdpIfCreateInterfaceSet(
    _In_ NET_IFINDEX IfIndex,
    _In_ VOID *InterfaceSetContext,
    _Out_ XDPIF_INTERFACE_SET_HANDLE *InterfaceSetHandle
    )
{
    NTSTATUS Status;
    XDP_INTERFACE_SET *IfSet = NULL;

    //
    // This function is invoked by an interface provider (e.g. NDIS6 via XdpLwf)
    // when a NIC is added.
    //

    TraceEnter(TRACE_CORE, "IfIndex=%u", IfIndex);

    ExAcquirePushLockExclusive(&XdpInterfaceSetsLock);

    //
    // Check for duplicate binding set.
    //
    if (XdpIfpFindIfSet(IfIndex) != NULL) {
        Status = STATUS_DUPLICATE_OBJECTID;
        goto Exit;
    }

    IfSet = ExAllocatePoolZero(PagedPool, sizeof(*IfSet), XDP_POOLTAG_IFSET);
    if (IfSet == NULL) {
        Status = STATUS_NO_MEMORY;
        goto Exit;
    }

    IfSet->IfIndex = IfIndex;
    IfSet->XdpIfInterfaceSetContext = InterfaceSetContext;
    InitializeListHead(&IfSet->Link);
    InsertTailList(&XdpInterfaceSets, &IfSet->Link);

    *InterfaceSetHandle = (XDPIF_INTERFACE_SET_HANDLE)IfSet;
    Status = STATUS_SUCCESS;

    TraceVerbose(
        TRACE_CORE, "IfIndex=%u XdpIfInterfaceSetContext=%p registered",
        IfSet->IfIndex, IfSet->XdpIfInterfaceSetContext);

Exit:

    ExReleasePushLockExclusive(&XdpInterfaceSetsLock);

    TraceExitStatus(TRACE_CORE);

    return Status;
}

_IRQL_requires_max_(PASSIVE_LEVEL)
VOID
XdpIfDeleteInterfaceSet(
    _In_ XDPIF_INTERFACE_SET_HANDLE InterfaceSetHandle
    )
{
    XDP_INTERFACE_SET *IfSet = (XDP_INTERFACE_SET *)InterfaceSetHandle;

    //
    // This function is invoked by an interface provider (e.g. XDP LWF)
    // when a NIC is deleted.
    //

    ExAcquirePushLockExclusive(&XdpInterfaceSetsLock);

    for (UINT32 Index = 0; Index < RTL_NUMBER_OF(IfSet->Interfaces); Index++) {
        FRE_ASSERT(IfSet->Interfaces[Index] == NULL);
    }

    RemoveEntryList(&IfSet->Link);
    ExFreePoolWithTag(IfSet, XDP_POOLTAG_IFSET);

    ExReleasePushLockExclusive(&XdpInterfaceSetsLock);
}

_IRQL_requires_max_(PASSIVE_LEVEL)
NTSTATUS
XdpIfAddInterfaces(
    _In_ XDPIF_INTERFACE_SET_HANDLE InterfaceSetHandle,
    _Inout_ XDP_ADD_INTERFACE *Interfaces,
    _In_ UINT32 InterfaceCount
    )
{
    NTSTATUS Status;
    XDP_INTERFACE_SET *IfSet = (XDP_INTERFACE_SET *)InterfaceSetHandle;

    //
    // This function is invoked by an interface provider (e.g. NDIS6 via XdpLwf)
    // when a NIC is added.
    //

    TraceEnter(TRACE_CORE, "IfIndex=%u", IfSet->IfIndex);

    ExAcquirePushLockExclusive(&XdpInterfaceSetsLock);

    for (UINT32 Index = 0; Index < InterfaceCount; Index++) {
        XDP_ADD_INTERFACE *AddIf = &Interfaces[Index];
        XDP_INTERFACE *Interface = NULL;

        if (!XdpValidateCapabilitiesEx(
                AddIf->InterfaceCapabilities->CapabilitiesEx,
                AddIf->InterfaceCapabilities->CapabilitiesSize)) {
            TraceError(
                TRACE_CORE, "IfIndex=%u Mode=%!XDP_MODE! Invalid capabilities",
                IfSet->IfIndex, AddIf->InterfaceCapabilities->Mode);
            Status = STATUS_NOT_SUPPORTED;
            goto Exit;
        }

        Interface = ExAllocatePoolZero(NonPagedPoolNx, sizeof(*Interface), XDP_POOLTAG_IF);
        if (Interface == NULL) {
            Status = STATUS_NO_MEMORY;
            goto Exit;
        }

        Interface->IfIndex = IfSet->IfIndex;
        Interface->IfSet = IfSet;
        Interface->RemoveInterfaceComplete = AddIf->RemoveInterfaceComplete;
        Interface->XdpIfInterfaceContext = AddIf->InterfaceContext;
        Interface->OpenConfig.Dispatch = &XdpOpenDispatch;
        Interface->ReferenceCount = 1;
        RtlCopyMemory(
            (XDP_CAPABILITIES_INTERNAL *)&Interface->Capabilities,
            AddIf->InterfaceCapabilities,
            sizeof(Interface->Capabilities));
        InitializeListHead(&Interface->Clients);

        Interface->WorkQueue =
            XdpCreateWorkQueue(XdpIfpInterfaceWorker, DISPATCH_LEVEL, XdpDriverObject, NULL);
        if (Interface->WorkQueue == NULL) {
            ExFreePoolWithTag(Interface, XDP_POOLTAG_IF);
            Status = STATUS_NO_MEMORY;
            goto Exit;
        }

        ASSERT(IfSet->Interfaces[Interface->Capabilities.Mode] == NULL);
        IfSet->Interfaces[Interface->Capabilities.Mode] = Interface;
        *AddIf->InterfaceHandle = (XDPIF_INTERFACE_HANDLE)Interface;

        TraceVerbose(
            TRACE_CORE, "IfIndex=%u Mode=%!XDP_MODE! XdpIfInterfaceContext=%p registered",
            Interface->IfIndex, Interface->Capabilities.Mode, Interface->XdpIfInterfaceContext);
    }

    Status = STATUS_SUCCESS;

Exit:

    if (!NT_SUCCESS(Status)) {
        for (UINT32 Index = 0; Index < InterfaceCount; Index++) {
            if (*Interfaces[Index].InterfaceHandle != NULL) {
                XDP_INTERFACE *Interface;
                Interface = (XDP_INTERFACE *)(*Interfaces[Index].InterfaceHandle);
                ASSERT(IfSet);
                IfSet->Interfaces[Interface->Capabilities.Mode] = NULL;
                *Interfaces[Index].InterfaceHandle = NULL;
                XdpIfpDereferenceInterface(Interface);
            }
        }
    }

    ExReleasePushLockExclusive(&XdpInterfaceSetsLock);

    TraceExitStatus(TRACE_CORE);

    return Status;
}

_IRQL_requires_max_(PASSIVE_LEVEL)
VOID
XdpIfRemoveInterfaces(
    _In_ XDPIF_INTERFACE_HANDLE *Interfaces,
    _In_ UINT32 InterfaceCount
    )
{
    //
    // This function is invoked by an interface provider (e.g. XDP LWF)
    // when a NIC is deleted.
    //

    ExAcquirePushLockExclusive(&XdpInterfaceSetsLock);

    for (UINT32 Index = 0; Index < InterfaceCount; Index++) {
        XDP_INTERFACE *Interface = (XDP_INTERFACE *)Interfaces[Index];

        TraceVerbose(
            TRACE_CORE, "IfIndex=%u Mode=%!XDP_MODE! deregistering",
            Interface->IfIndex, Interface->Capabilities.Mode);
        Interface->IfSet->Interfaces[Interface->Capabilities.Mode] = NULL;
        Interface->IfSet = NULL;

        Interface->DeleteWorkItem.BindingHandle = (XDP_BINDING_HANDLE)Interface;
        Interface->DeleteWorkItem.WorkRoutine = XdpIfpInterfaceDelete;
        XdpIfQueueWorkItem(&Interface->DeleteWorkItem);
    }

    ExReleasePushLockExclusive(&XdpInterfaceSetsLock);
}

_IRQL_requires_(PASSIVE_LEVEL)
VOID
XdpIfInitializeClientEntry(
    _Out_ XDP_BINDING_CLIENT_ENTRY *ClientEntry
    )
{
    RtlZeroMemory(ClientEntry, sizeof(*ClientEntry));
    InitializeListHead(&ClientEntry->Link);
}

_IRQL_requires_(PASSIVE_LEVEL)
NTSTATUS
XdpIfRegisterClient(
    _In_ XDP_BINDING_HANDLE BindingHandle,
    _In_ CONST XDP_BINDING_CLIENT *Client,
    _In_ CONST VOID *Key,
    _In_ XDP_BINDING_CLIENT_ENTRY *ClientEntry
    )
{
    XDP_INTERFACE *Interface = (XDP_INTERFACE *)BindingHandle;
    LIST_ENTRY *Entry;

    FRE_ASSERT(Client->ClientId != XDP_BINDING_CLIENT_ID_INVALID);
    FRE_ASSERT(Client->KeySize > 0);
    FRE_ASSERT(Key != NULL);

    if (Interface->BindingDeleting) {
        TraceInfo(
            TRACE_CORE, "IfIndex=%u Mode=%!XDP_MODE! client registration failed: binding deleting",
            Interface->IfIndex, Interface->Capabilities.Mode);
        return STATUS_DELETE_PENDING;
    }

    //
    // Verify we're not inserting a duplicate client.
    //
    Entry = Interface->Clients.Flink;
    while (Entry != &Interface->Clients) {
        XDP_BINDING_CLIENT_ENTRY *Candidate =
            CONTAINING_RECORD(Entry, XDP_BINDING_CLIENT_ENTRY, Link);
        Entry = Entry->Flink;

        if (!NT_VERIFY(
                (Candidate->Client->ClientId != Client->ClientId) ||
                !RtlEqualMemory(Candidate->Key, Key, Client->KeySize))) {
            TraceInfo(
                TRACE_CORE,
                "IfIndex=%u Mode=%!XDP_MODE! client registration failed: duplicate client",
                Interface->IfIndex, Interface->Capabilities.Mode);
            return STATUS_DUPLICATE_OBJECTID;
        }
    }

    ClientEntry->Client = Client;
    ClientEntry->Key = Key;
    XdpIfpReferenceInterface(Interface);
    InsertTailList(&Interface->Clients, &ClientEntry->Link);

    return STATUS_SUCCESS;
}

_IRQL_requires_(PASSIVE_LEVEL)
VOID
XdpIfDeregisterClient(
    _In_ XDP_BINDING_HANDLE BindingHandle,
    _In_ XDP_BINDING_CLIENT_ENTRY *ClientEntry
    )
{
    XDP_INTERFACE *Interface = (XDP_INTERFACE *)BindingHandle;

    //
    // Invoked by XDP components (e.g. programs, XSKs) to detach from an
    // interface binding.
    //

    if (!IsListEmpty(&ClientEntry->Link)) {
        RemoveEntryList(&ClientEntry->Link);
        InitializeListHead(&ClientEntry->Link);
        XdpIfpDereferenceInterface(Interface);
    }
}

_IRQL_requires_(PASSIVE_LEVEL)
XDP_BINDING_CLIENT_ENTRY *
XdpIfFindClientEntry(
    _In_ XDP_BINDING_HANDLE BindingHandle,
    _In_ CONST XDP_BINDING_CLIENT *Client,
    _In_ CONST VOID *Key
    )
{
    XDP_INTERFACE *Interface = (XDP_INTERFACE *)BindingHandle;
    LIST_ENTRY *Entry;
    XDP_BINDING_CLIENT_ENTRY *Candidate;

    Entry = Interface->Clients.Flink;
    while (Entry != &Interface->Clients) {
        Candidate = CONTAINING_RECORD(Entry, XDP_BINDING_CLIENT_ENTRY, Link);
        Entry = Entry->Flink;

        if (Candidate->Client->ClientId == Client->ClientId &&
            RtlEqualMemory(Candidate->Key, Key, Client->KeySize)) {
            return Candidate;
        }
    }

    return NULL;
}

_IRQL_requires_(PASSIVE_LEVEL)
NET_IFINDEX
XdpIfGetIfIndex(
    _In_ XDP_BINDING_HANDLE BindingHandle
    )
{
    XDP_INTERFACE *Interface = (XDP_INTERFACE *)BindingHandle;

    return Interface->IfIndex;
}

static
_IRQL_requires_(PASSIVE_LEVEL)
NTSTATUS
XdpIfpReferenceProvider(
    _In_ XDP_INTERFACE *Interface
    )
{
    NTSTATUS Status;

    if (Interface->Rundown) {
        Status = STATUS_DELETE_PENDING;
        TraceInfo(
            TRACE_CORE, "IfIndex=%u Mode=%!XDP_MODE! reference failed: rundown",
            Interface->IfIndex, Interface->Capabilities.Mode);
        goto Exit;
    }

    if (Interface->InterfaceContext == NULL) {
        ASSERT(Interface->ProviderReference == 0);
        Status = XdpIfpOpenInterface(Interface);
        if (!NT_SUCCESS(Status)) {
            TraceInfo(
                TRACE_CORE, "IfIndex=%u Mode=%!XDP_MODE! reference failed: open interface",
                Interface->IfIndex, Interface->Capabilities.Mode);
            goto Exit;
        }
    }

    Interface->ProviderReference++;
    Status = STATUS_SUCCESS;

Exit:

    return Status;
}

static
_IRQL_requires_(PASSIVE_LEVEL)
VOID
XdpIfpDereferenceProvider(
    _In_ XDP_INTERFACE *Interface
    )
{
    if (--Interface->ProviderReference == 0) {
        XdpIfpCloseInterface(Interface);
    }
}

static
_IRQL_requires_(PASSIVE_LEVEL)
NTSTATUS
XdpIfpInvokeCreateRxQueue(
    _In_ XDP_INTERFACE *Interface,
    _Inout_ XDP_RX_QUEUE_CONFIG_CREATE Config,
    _Out_ XDP_INTERFACE_HANDLE *InterfaceRxQueue,
    _Out_ CONST XDP_INTERFACE_RX_QUEUE_DISPATCH **InterfaceRxQueueDispatch
    )
{
    NTSTATUS Status;

    TraceEnter(
        TRACE_CORE, "IfIndex=%u Mode=%!XDP_MODE! QueueId=%u",
        Interface->IfIndex, Interface->Capabilities.Mode,
        XdpRxQueueGetTargetQueueInfo(Config)->QueueId);

    Status =
        Interface->InterfaceDispatch->CreateRxQueue(
            Interface->InterfaceContext, Config, InterfaceRxQueue, InterfaceRxQueueDispatch);

    TraceExitStatus(TRACE_CORE);

    return Status;
}

_IRQL_requires_(PASSIVE_LEVEL)
NTSTATUS
XdpIfCreateRxQueue(
    _In_ XDP_BINDING_HANDLE BindingHandle,
    _Inout_ XDP_RX_QUEUE_CONFIG_CREATE Config,
    _Out_ XDP_INTERFACE_HANDLE *InterfaceRxQueue,
    _Out_ CONST XDP_INTERFACE_RX_QUEUE_DISPATCH **InterfaceRxQueueDispatch
    )
{
    XDP_INTERFACE *Interface = (XDP_INTERFACE *)BindingHandle;
    NTSTATUS Status;

    TraceEnter(
        TRACE_CORE, "IfIndex=%u Mode=%!XDP_MODE! QueueId=%u",
        Interface->IfIndex, Interface->Capabilities.Mode,
        XdpRxQueueGetTargetQueueInfo(Config)->QueueId);

    *InterfaceRxQueue = NULL;
    *InterfaceRxQueueDispatch = NULL;

    Status = XdpIfpReferenceProvider(Interface);
    if (!NT_SUCCESS(Status)) {
        goto Exit;
    }

    Status = XdpIfpInvokeCreateRxQueue(Interface, Config, InterfaceRxQueue, InterfaceRxQueueDispatch);
    if (!NT_SUCCESS(Status)) {
        XdpIfpDereferenceProvider(Interface);
        goto Exit;
    }

    FRE_ASSERT(*InterfaceRxQueue != NULL);
    FRE_ASSERT(*InterfaceRxQueueDispatch != NULL);
    TraceVerbose(TRACE_CORE, "Created InterfaceQueue=%p", *InterfaceRxQueue);

Exit:

    TraceExitStatus(TRACE_CORE);

    return Status;
}

_IRQL_requires_(PASSIVE_LEVEL)
VOID
XdpIfActivateRxQueue(
    _In_ XDP_BINDING_HANDLE BindingHandle,
    _In_ XDP_INTERFACE_HANDLE InterfaceRxQueue,
    _In_ XDP_RX_QUEUE_HANDLE XdpRxQueue,
    _In_ XDP_RX_QUEUE_CONFIG_ACTIVATE Config
    )
{
    XDP_INTERFACE *Interface = (XDP_INTERFACE *)BindingHandle;

    TraceEnter(
        TRACE_CORE, "IfIndex=%u Mode=%!XDP_MODE! InterfaceQueue=%p",
        Interface->IfIndex, Interface->Capabilities.Mode, InterfaceRxQueue);

    Interface->InterfaceDispatch->ActivateRxQueue(InterfaceRxQueue, XdpRxQueue, Config);

    TraceExit(TRACE_CORE);
}

_IRQL_requires_(PASSIVE_LEVEL)
VOID
XdpIfDeleteRxQueue(
    _In_ XDP_BINDING_HANDLE BindingHandle,
    _In_ XDP_INTERFACE_HANDLE InterfaceRxQueue
    )
{
    XDP_INTERFACE *Interface = (XDP_INTERFACE *)BindingHandle;

    TraceEnter(
        TRACE_CORE, "IfIndex=%u Mode=%!XDP_MODE! InterfaceQueue=%p",
        Interface->IfIndex, Interface->Capabilities.Mode, InterfaceRxQueue);

    Interface->InterfaceDispatch->DeleteRxQueue(InterfaceRxQueue);

    XdpIfpDereferenceProvider(Interface);

    TraceExit(TRACE_CORE);
}

static
_IRQL_requires_(PASSIVE_LEVEL)
NTSTATUS
XdpIfpInvokeCreateTxQueue(
    _In_ XDP_INTERFACE *Interface,
    _Inout_ XDP_TX_QUEUE_CONFIG_CREATE Config,
    _Out_ XDP_INTERFACE_HANDLE *InterfaceTxQueue,
    _Out_ CONST XDP_INTERFACE_TX_QUEUE_DISPATCH **InterfaceTxQueueDispatch
    )
{
    NTSTATUS Status;

    TraceEnter(
        TRACE_CORE, "IfIndex=%u Mode=%!XDP_MODE! QueueId=%u",
        Interface->IfIndex, Interface->Capabilities.Mode,
        XdpTxQueueGetTargetQueueInfo(Config)->QueueId);

    Status =
        Interface->InterfaceDispatch->CreateTxQueue(
            Interface->InterfaceContext, Config, InterfaceTxQueue, InterfaceTxQueueDispatch);

    TraceExitStatus(TRACE_CORE);

    return Status;
}

_IRQL_requires_(PASSIVE_LEVEL)
NTSTATUS
XdpIfCreateTxQueue(
    _In_ XDP_BINDING_HANDLE BindingHandle,
    _Inout_ XDP_TX_QUEUE_CONFIG_CREATE Config,
    _Out_ XDP_INTERFACE_HANDLE *InterfaceTxQueue,
    _Out_ CONST XDP_INTERFACE_TX_QUEUE_DISPATCH **InterfaceTxQueueDispatch
    )
{
    XDP_INTERFACE *Interface = (XDP_INTERFACE *)BindingHandle;
    NTSTATUS Status;

    TraceEnter(
        TRACE_CORE, "IfIndex=%u Mode=%!XDP_MODE! QueueId=%u",
        Interface->IfIndex, Interface->Capabilities.Mode,
        XdpTxQueueGetTargetQueueInfo(Config)->QueueId);

    *InterfaceTxQueue = NULL;
    *InterfaceTxQueueDispatch = NULL;

    Status = XdpIfpReferenceProvider(Interface);
    if (!NT_SUCCESS(Status)) {
        goto Exit;
    }

    Status = XdpIfpInvokeCreateTxQueue(Interface, Config, InterfaceTxQueue, InterfaceTxQueueDispatch);
    if (!NT_SUCCESS(Status)) {
        XdpIfpDereferenceProvider(Interface);
        goto Exit;
    }

    FRE_ASSERT(*InterfaceTxQueue != NULL);
    FRE_ASSERT(*InterfaceTxQueueDispatch != NULL);
    TraceVerbose(TRACE_CORE, "Created InterfaceQueue=%p", *InterfaceTxQueue);

Exit:

    TraceExitStatus(TRACE_CORE);

    return Status;
}

_IRQL_requires_(PASSIVE_LEVEL)
VOID
XdpIfActivateTxQueue(
    _In_ XDP_BINDING_HANDLE BindingHandle,
    _In_ XDP_INTERFACE_HANDLE InterfaceTxQueue,
    _In_ XDP_TX_QUEUE_HANDLE XdpTxQueue,
    _In_ XDP_TX_QUEUE_CONFIG_ACTIVATE Config
    )
{
    XDP_INTERFACE *Interface = (XDP_INTERFACE *)BindingHandle;

    TraceEnter(
        TRACE_CORE, "IfIndex=%u Mode=%!XDP_MODE! InterfaceQueue=%p",
        Interface->IfIndex, Interface->Capabilities.Mode, InterfaceTxQueue);

    Interface->InterfaceDispatch->ActivateTxQueue(InterfaceTxQueue, XdpTxQueue, Config);

    TraceExit(TRACE_CORE);
}

_IRQL_requires_(PASSIVE_LEVEL)
VOID
XdpIfDeleteTxQueue(
    _In_ XDP_BINDING_HANDLE BindingHandle,
    _In_ XDP_INTERFACE_HANDLE InterfaceTxQueue
    )
{
    XDP_INTERFACE *Interface = (XDP_INTERFACE *)BindingHandle;

    TraceEnter(
        TRACE_CORE, "IfIndex=%u Mode=%!XDP_MODE! InterfaceQueue=%p",
        Interface->IfIndex, Interface->Capabilities.Mode, InterfaceTxQueue);

    Interface->InterfaceDispatch->DeleteTxQueue(InterfaceTxQueue);

    XdpIfpDereferenceProvider(Interface);

    TraceExit(TRACE_CORE);
}

_IRQL_requires_(PASSIVE_LEVEL)
_IRQL_requires_same_
NTSTATUS
XdpIfStart(
    VOID
    )
{
    ExInitializePushLock(&XdpInterfaceSetsLock);
    InitializeListHead(&XdpInterfaceSets);
    XdpBindInitialized = TRUE;
    return STATUS_SUCCESS;
}

_IRQL_requires_(PASSIVE_LEVEL)
_IRQL_requires_same_
VOID
XdpIfStop(
    VOID
    )
{
    if (!XdpBindInitialized) {
        return;
    }

    ExAcquirePushLockExclusive(&XdpInterfaceSetsLock);

    ASSERT(IsListEmpty(&XdpInterfaceSets));

    ExReleasePushLockExclusive(&XdpInterfaceSetsLock);
}
