#include "driver.h"

#ifdef ALLOC_PRAGMA
#pragma alloc_text (PAGE, VirtualMouseQueueInitialize)
#endif

NTSTATUS
VirtualMouseQueueInitialize(
    _In_ WDFDEVICE Device
    )
{
    WDFQUEUE              queue;
    NTSTATUS              status;
    WDF_IO_QUEUE_CONFIG   queueConfig;

    PAGED_CODE();

    WDF_IO_QUEUE_CONFIG_INIT_DEFAULT_QUEUE(
        &queueConfig,
        WdfIoQueueDispatchParallel);

    queueConfig.EvtIoDeviceControl = VirtualMouseEvtIoDeviceControl;

    status = WdfIoQueueCreate(
        Device,
        &queueConfig,
        WDF_NO_OBJECT_ATTRIBUTES,
        &queue);

    return status;
}

VOID
VirtualMouseEvtIoDeviceControl(
    _In_ WDFQUEUE   Queue,
    _In_ WDFREQUEST Request,
    _In_ size_t     OutputBufferLength,
    _In_ size_t     InputBufferLength,
    _In_ ULONG      IoControlCode
    )
{
    NTSTATUS             status;
    WDFDEVICE            device;
    PDEVICE_CONTEXT      deviceContext;
    PVOID                inputBuffer       = NULL;
    size_t               actualLength      = 0;
    MOUSE_INPUT_REPORT   incoming;
    BOOLEAN              submitNow         = FALSE;
    MOUSE_INPUT_REPORT   reportForSubmit;

    UNREFERENCED_PARAMETER(OutputBufferLength);

    device        = WdfIoQueueGetDevice(Queue);
    deviceContext = DeviceGetContext(device);

    switch (IoControlCode) {

    case IOCTL_INJECT_MOUSE:

        if (InputBufferLength != sizeof(MOUSE_INPUT_REPORT)) {
            status = STATUS_INVALID_BUFFER_SIZE;
            break;
        }

        status = WdfRequestRetrieveInputBuffer(
            Request,
            sizeof(MOUSE_INPUT_REPORT),
            &inputBuffer,
            &actualLength);

        if (!NT_SUCCESS(status)) {
            break;
        }

        RtlCopyMemory(&incoming, inputBuffer, sizeof(MOUSE_INPUT_REPORT));
        incoming.reportId = 1;

        VirtualMouseEnqueueReport(
            deviceContext,
            &incoming,
            &submitNow,
            &reportForSubmit);

        if (submitNow) {
            HID_XFER_PACKET    packet;
            UCHAR              buffer[sizeof(MOUSE_INPUT_REPORT)];

            RtlCopyMemory(buffer, &reportForSubmit, sizeof(buffer));

            packet.reportId        = reportForSubmit.reportId;
            packet.reportBuffer    = buffer;
            packet.reportBufferLen = sizeof(buffer);

            VhfReadReportSubmit(deviceContext->VhfHandle, &packet);
        }

        status = STATUS_SUCCESS;
        break;

    default:
        status = STATUS_INVALID_DEVICE_REQUEST;
        break;
    }

    WdfRequestComplete(Request, status);
}

_IRQL_requires_max_(DISPATCH_LEVEL)
VOID
VirtualMouseEnqueueReport(
    _In_  PDEVICE_CONTEXT             DeviceContext,
    _In_  const MOUSE_INPUT_REPORT*   Report,
    _Out_ BOOLEAN*                    OutSubmitNow,
    _Out_ MOUSE_INPUT_REPORT*         OutReportForSubmit
    )
{
    KIRQL oldIrql;

    *OutSubmitNow = FALSE;
    RtlZeroMemory(OutReportForSubmit, sizeof(*OutReportForSubmit));

    KeAcquireSpinLock(&DeviceContext->ReportLock, &oldIrql);

    if (DeviceContext->PendingRead) {
        DeviceContext->PendingRead = FALSE;
        *OutSubmitNow              = TRUE;
        *OutReportForSubmit        = *Report;
        KeReleaseSpinLock(&DeviceContext->ReportLock, oldIrql);
        return;
    }

    if (DeviceContext->Count == VMOUSE_REPORT_QUEUE_DEPTH) {
        DeviceContext->Tail =
            (DeviceContext->Tail + 1) % VMOUSE_REPORT_QUEUE_DEPTH;
        DeviceContext->Count--;
    }

    DeviceContext->Reports[DeviceContext->Head] = *Report;
    DeviceContext->Head =
        (DeviceContext->Head + 1) % VMOUSE_REPORT_QUEUE_DEPTH;
    DeviceContext->Count++;

    KeReleaseSpinLock(&DeviceContext->ReportLock, oldIrql);
}

_IRQL_requires_max_(DISPATCH_LEVEL)
BOOLEAN
VirtualMouseDequeueReport(
    _In_  PDEVICE_CONTEXT             DeviceContext,
    _Out_ MOUSE_INPUT_REPORT*         OutReport
    )
{
    KIRQL    oldIrql;
    BOOLEAN  result;

    RtlZeroMemory(OutReport, sizeof(*OutReport));

    KeAcquireSpinLock(&DeviceContext->ReportLock, &oldIrql);

    if (DeviceContext->Count == 0) {
        DeviceContext->PendingRead = TRUE;
        result                     = FALSE;
    } else {
        *OutReport          = DeviceContext->Reports[DeviceContext->Tail];
        DeviceContext->Tail =
            (DeviceContext->Tail + 1) % VMOUSE_REPORT_QUEUE_DEPTH;
        DeviceContext->Count--;
        result              = TRUE;
    }

    KeReleaseSpinLock(&DeviceContext->ReportLock, oldIrql);
    return result;
}
