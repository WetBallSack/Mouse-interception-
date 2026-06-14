#ifndef VIRTUALMOUSE_DRIVER_H_
#define VIRTUALMOUSE_DRIVER_H_

#include <ntddk.h>
#include <wdf.h>
#include <vhf.h>

#include "..\shared\driver_shared.h"

#define VMOUSE_REPORT_QUEUE_DEPTH   64
#define VMOUSE_POOL_TAG             'uoMV'

typedef struct _DEVICE_CONTEXT {
    VHFHANDLE                  VhfHandle;
    BOOLEAN                    VhfStarted;
    KSPIN_LOCK                 ReportLock;
    MOUSE_INPUT_REPORT         Reports[VMOUSE_REPORT_QUEUE_DEPTH];
    ULONG                      Head;
    ULONG                      Tail;
    ULONG                      Count;
    BOOLEAN                    PendingRead;
} DEVICE_CONTEXT, *PDEVICE_CONTEXT;

WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(DEVICE_CONTEXT, DeviceGetContext)

DRIVER_INITIALIZE                    DriverEntry;
EVT_WDF_DRIVER_DEVICE_ADD            VirtualMouseEvtDeviceAdd;
EVT_WDF_OBJECT_CONTEXT_CLEANUP       VirtualMouseEvtDeviceContextCleanup;

NTSTATUS
VirtualMouseQueueInitialize(
    _In_ WDFDEVICE Device
    );

EVT_WDF_IO_QUEUE_IO_DEVICE_CONTROL   VirtualMouseEvtIoDeviceControl;
EVT_VHF_ASYNC_OPERATION              VirtualMouseEvtVhfReadyForNextReadReport;

_IRQL_requires_max_(DISPATCH_LEVEL)
VOID
VirtualMouseEnqueueReport(
    _In_  PDEVICE_CONTEXT             DeviceContext,
    _In_  const MOUSE_INPUT_REPORT*   Report,
    _Out_ BOOLEAN*                    OutSubmitNow,
    _Out_ MOUSE_INPUT_REPORT*         OutReportForSubmit
    );

_IRQL_requires_max_(DISPATCH_LEVEL)
BOOLEAN
VirtualMouseDequeueReport(
    _In_  PDEVICE_CONTEXT             DeviceContext,
    _Out_ MOUSE_INPUT_REPORT*         OutReport
    );

#endif
