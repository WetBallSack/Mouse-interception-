#include "driver.h"

#ifdef ALLOC_PRAGMA
#pragma alloc_text (INIT, DriverEntry)
#pragma alloc_text (PAGE, VirtualMouseEvtDeviceAdd)
#pragma alloc_text (PAGE, VirtualMouseEvtDeviceContextCleanup)
#endif

static const UCHAR MouseReportDescriptor[] = {
    0x05, 0x01,
    0x09, 0x02,
    0xA1, 0x01,
    0x09, 0x01,
    0xA1, 0x00,
    0x85, 0x01,
    0x05, 0x09,
    0x19, 0x01,
    0x29, 0x03,
    0x15, 0x00,
    0x25, 0x01,
    0x95, 0x03,
    0x75, 0x01,
    0x81, 0x02,
    0x95, 0x01,
    0x75, 0x05,
    0x81, 0x03,
    0x05, 0x01,
    0x09, 0x30,
    0x09, 0x31,
    0x16, 0x00, 0x80,
    0x26, 0xFF, 0x7F,
    0x75, 0x10,
    0x95, 0x02,
    0x81, 0x06,
    0x09, 0x38,
    0x15, 0x81,
    0x25, 0x7F,
    0x75, 0x08,
    0x95, 0x01,
    0x81, 0x06,
    0xC0,
    0xC0
};

DECLARE_CONST_UNICODE_STRING(
    VirtualMouseSDDL,
    L"D:P(A;;GA;;;SY)(A;;GA;;;BA)(A;;GRGW;;;IU)"
);

DECLARE_CONST_UNICODE_STRING(
    VirtualMouseSymbolicLink,
    VIRTUAL_MOUSE_SYMLINK_NAME
);

NTSTATUS
DriverEntry(
    _In_ PDRIVER_OBJECT  DriverObject,
    _In_ PUNICODE_STRING RegistryPath
    )
{
    WDF_DRIVER_CONFIG  config;
    NTSTATUS           status;

    WDF_DRIVER_CONFIG_INIT(&config, VirtualMouseEvtDeviceAdd);

    status = WdfDriverCreate(
        DriverObject,
        RegistryPath,
        WDF_NO_OBJECT_ATTRIBUTES,
        &config,
        WDF_NO_HANDLE);

    return status;
}

NTSTATUS
VirtualMouseEvtDeviceAdd(
    _In_    WDFDRIVER       Driver,
    _Inout_ PWDFDEVICE_INIT DeviceInit
    )
{
    NTSTATUS                status;
    WDF_OBJECT_ATTRIBUTES   deviceAttributes;
    WDFDEVICE               device;
    PDEVICE_CONTEXT         deviceContext;
    VHF_CONFIG              vhfConfig;

    UNREFERENCED_PARAMETER(Driver);

    PAGED_CODE();

    WdfDeviceInitSetCharacteristics(DeviceInit, FILE_DEVICE_SECURE_OPEN, FALSE);

    status = WdfDeviceInitAssignSDDLString(DeviceInit, &VirtualMouseSDDL);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&deviceAttributes, DEVICE_CONTEXT);
    deviceAttributes.EvtCleanupCallback = VirtualMouseEvtDeviceContextCleanup;

    status = WdfDeviceCreate(&DeviceInit, &deviceAttributes, &device);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    deviceContext = DeviceGetContext(device);
    RtlZeroMemory(deviceContext, sizeof(*deviceContext));
    KeInitializeSpinLock(&deviceContext->ReportLock);

    status = WdfDeviceCreateSymbolicLink(device, &VirtualMouseSymbolicLink);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    status = VirtualMouseQueueInitialize(device);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    VHF_CONFIG_INIT(
        &vhfConfig,
        WdfDeviceWdmGetDeviceObject(device),
        sizeof(MouseReportDescriptor),
        (PUCHAR)MouseReportDescriptor);

    vhfConfig.EvtVhfReadyForNextReadReport =
        VirtualMouseEvtVhfReadyForNextReadReport;
    vhfConfig.VhfClientContext             = deviceContext;

    status = VhfCreate(&vhfConfig, &deviceContext->VhfHandle);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    status = VhfStart(deviceContext->VhfHandle);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    deviceContext->VhfStarted = TRUE;
    return STATUS_SUCCESS;
}

VOID
VirtualMouseEvtDeviceContextCleanup(
    _In_ WDFOBJECT Object
    )
{
    PDEVICE_CONTEXT deviceContext;

    PAGED_CODE();

    deviceContext = DeviceGetContext((WDFDEVICE)Object);

    if (deviceContext->VhfHandle != NULL) {
        VhfDelete(deviceContext->VhfHandle, TRUE);
        deviceContext->VhfHandle = NULL;
    }
}

VOID
VirtualMouseEvtVhfReadyForNextReadReport(
    _In_ PVOID            VhfClientContext,
    _In_ VHFOPERATIONHANDLE VhfOperationHandle,
    _In_ PVOID            VhfOperationContext,
    _In_ PHID_XFER_PACKET HidTransferPacket
    )
{
    PDEVICE_CONTEXT     deviceContext = (PDEVICE_CONTEXT)VhfClientContext;
    MOUSE_INPUT_REPORT  report;
    BOOLEAN             have;

    UNREFERENCED_PARAMETER(VhfOperationContext);
    UNREFERENCED_PARAMETER(VhfOperationHandle);

    have = VirtualMouseDequeueReport(deviceContext, &report);
    if (!have) {
        return;
    }

    if (HidTransferPacket->reportBufferLen >= sizeof(MOUSE_INPUT_REPORT)) {
        HidTransferPacket->reportId = report.reportId;
        RtlCopyMemory(
            HidTransferPacket->reportBuffer,
            &report,
            sizeof(MOUSE_INPUT_REPORT));

        VhfReadReportSubmit(deviceContext->VhfHandle, HidTransferPacket);
    } else {
        KIRQL oldIrql;
        KeAcquireSpinLock(&deviceContext->ReportLock, &oldIrql);
        deviceContext->PendingRead = TRUE;
        KeReleaseSpinLock(&deviceContext->ReportLock, oldIrql);
    }
}
