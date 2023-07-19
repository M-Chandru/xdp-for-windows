//
// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.
//

#ifndef XDPAPI_H
#define XDPAPI_H

#ifdef __cplusplus
extern "C" {
#endif

#include <xdp/hookid.h>
#include <xdp/objectheader.h>
#include <xdp/program.h>

#ifndef XDPAPI
#define XDPAPI __declspec(dllimport)
#endif

//
// Create and attach an XDP program to an interface. The caller may optionally
// specify generic or native XDP binding mode. See xdp/program.h for placeholder
// program definitions.
//
// N.B. The current implementation supports only L2 RX inspect programs.
//

typedef enum _XDP_CREATE_PROGRAM_FLAGS {
    XDP_CREATE_PROGRAM_FLAG_NONE = 0x0,

    //
    // Attach to the interface using the generic XDP provider.
    //
    XDP_CREATE_PROGRAM_FLAG_GENERIC = 0x1,

    //
    // Attach to the interface using the native XDP provider. If the interface does
    // not support native XDP, the attach will fail.
    //
    XDP_CREATE_PROGRAM_FLAG_NATIVE = 0x2,

    //
    // Attach to all XDP queues on the interface.
    //
    XDP_CREATE_PROGRAM_FLAG_ALL_QUEUES = 0x4,
} XDP_CREATE_PROGRAM_FLAGS;

DEFINE_ENUM_FLAG_OPERATORS(XDP_CREATE_PROGRAM_FLAGS);
C_ASSERT(sizeof(XDP_CREATE_PROGRAM_FLAGS) == sizeof(UINT32));

typedef
HRESULT
XDP_CREATE_PROGRAM_FN(
    _In_ UINT32 InterfaceIndex,
    _In_ CONST XDP_HOOK_ID *HookId,
    _In_ UINT32 QueueId,
    _In_ XDP_CREATE_PROGRAM_FLAGS Flags,
    _In_reads_(RuleCount) CONST XDP_RULE *Rules,
    _In_ UINT32 RuleCount,
    _Out_ HANDLE *Program
    );

//
// Interface API.
//

//
// Open a handle to get/set offloads/configurations/properties on an interface.
//
typedef
HRESULT
XDP_INTERFACE_OPEN_FN(
    _In_ UINT32 InterfaceIndex,
    _Out_ HANDLE *InterfaceHandle
    );

#include "afxdp.h"

typedef struct _XDP_API_TABLE XDP_API_TABLE;

//
// The only API version currently supported. Any change to the API is considered
// a breaking change and support for previous versions will be removed.
//
#define XDP_VERSION_PRERELEASE 100007

//
// Opens the API and returns an API function table with the rest of the API's
// functions. Each open must invoke a corresponding XdpCloseApi when the API
// will no longer be used.
//
typedef
HRESULT
XDP_OPEN_API_FN(
    _In_ UINT32 XdpApiVersion,
    _Out_ CONST XDP_API_TABLE **XdpApiTable
    );

XDPAPI XDP_OPEN_API_FN XdpOpenApi;

//
// Releases the reference to the API returned by XdpOpenApi.
//
typedef
VOID
XDP_CLOSE_API_FN(
    _In_ CONST XDP_API_TABLE *XdpApiTable
    );

XDPAPI XDP_CLOSE_API_FN XdpCloseApi;

typedef
VOID *
XDP_GET_ROUTINE_FN(
    _In_z_ const CHAR *RoutineName
    );

typedef struct _XDP_API_TABLE {
    XDP_OPEN_API_FN *XdpOpenApi;
    XDP_CLOSE_API_FN *XdpCloseApi;
    XDP_GET_ROUTINE_FN *XdpGetRoutine;
    XDP_CREATE_PROGRAM_FN *XdpCreateProgram;
    XDP_INTERFACE_OPEN_FN *XdpInterfaceOpen;
    XSK_CREATE_FN *XskCreate;
    XSK_BIND_FN *XskBind;
    XSK_ACTIVATE_FN *XskActivate;
    XSK_NOTIFY_SOCKET_FN *XskNotifySocket;
    XSK_NOTIFY_ASYNC_FN *XskNotifyAsync;
    XSK_GET_NOTIFY_ASYNC_RESULT_FN *XskGetNotifyAsyncResult;
    XSK_SET_SOCKOPT_FN *XskSetSockopt;
    XSK_GET_SOCKOPT_FN *XskGetSockopt;
    XSK_IOCTL_FN *XskIoctl;
} XDP_API_TABLE;

typedef struct _XDP_LOAD_CONTEXT *XDP_LOAD_API_CONTEXT;

#if !defined(_KERNEL_MODE)

//
// Dynamically loads XDP, then opens the API and returns an API function table
// with the rest of the API's functions. Each open must invoke a corresponding
// XdpCloseApi when the API will no longer be used.
//
// This routine cannot be called from DllMain.
//
inline
HRESULT
XdpLoadApi(
    _In_ UINT32 XdpApiVersion,
    _Out_ XDP_LOAD_API_CONTEXT *XdpLoadApiContext,
    _Out_ CONST XDP_API_TABLE **XdpApiTable
    )
{
    HRESULT Result;
    HMODULE XdpHandle;
    XDP_OPEN_API_FN *OpenApi;

    *XdpLoadApiContext = NULL;
    *XdpApiTable = NULL;

    XdpHandle = LoadLibraryA("xdpapi.dll");
    if (XdpHandle == NULL) {
        Result = E_NOINTERFACE;
        goto Exit;
    }

    OpenApi = (XDP_OPEN_API_FN *)GetProcAddress(XdpHandle, "XdpOpenApi");
    if (OpenApi == NULL) {
        Result = E_NOINTERFACE;
        goto Exit;
    }

    Result = OpenApi(XdpApiVersion, XdpApiTable);

Exit:

    if (SUCCEEDED(Result)) {
        *XdpLoadApiContext = (XDP_LOAD_API_CONTEXT)XdpHandle;
    } else {
        if (XdpHandle != NULL) {
            FreeLibrary(XdpHandle);
        }
    }

    return Result;
}

//
// Releases the reference to the API returned by XdpOpenApi, then dynamically
// unloads XDP.
//
// This routine cannot be called from DllMain.
//
inline
VOID
XdpUnloadApi(
    _In_ XDP_LOAD_API_CONTEXT XdpLoadApiContext,
    _In_ CONST XDP_API_TABLE *XdpApiTable
    )
{
    HMODULE XdpHandle = (HMODULE)XdpLoadApiContext;

    XdpApiTable->XdpCloseApi(XdpApiTable);

    FreeLibrary(XdpHandle);
}

#endif // !defined(_KERNEL_MODE)

#ifdef __cplusplus
} // extern "C"
#endif

#endif
