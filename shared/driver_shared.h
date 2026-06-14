#ifndef VIRTUALMOUSE_DRIVER_SHARED_H_
#define VIRTUALMOUSE_DRIVER_SHARED_H_

#ifdef __cplusplus
extern "C" {
#endif

#pragma pack(push, 1)
typedef struct _MOUSE_INPUT_REPORT {
    UCHAR  reportId;
    UCHAR  buttons;
    SHORT  x;
    SHORT  y;
    CHAR   wheel;
} MOUSE_INPUT_REPORT, *PMOUSE_INPUT_REPORT;
#pragma pack(pop)

#define VMOUSE_BTN_LEFT    0x01u
#define VMOUSE_BTN_RIGHT   0x02u
#define VMOUSE_BTN_MIDDLE  0x04u

#define IOCTL_INJECT_MOUSE \
    CTL_CODE(FILE_DEVICE_UNKNOWN, 0x800, METHOD_BUFFERED, FILE_ANY_ACCESS)

#define VIRTUAL_MOUSE_USERMODE_PATH   L"\\\\.\\VirtualMouse"
#define VIRTUAL_MOUSE_SYMLINK_NAME    L"\\DosDevices\\VirtualMouse"

#ifdef __cplusplus
}
#endif

#endif
