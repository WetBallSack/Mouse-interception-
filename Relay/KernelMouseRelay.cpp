#include "KernelMouseRelay.h"

KernelMouseRelay::KernelMouseRelay()
    : hDevice_(INVALID_HANDLE_VALUE)
{
    hDevice_ = CreateFileW(
        VIRTUAL_MOUSE_USERMODE_PATH,
        GENERIC_READ | GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        nullptr,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL,
        nullptr);
}

KernelMouseRelay::~KernelMouseRelay()
{
    if (hDevice_ != INVALID_HANDLE_VALUE) {
        CloseHandle(hDevice_);
        hDevice_ = INVALID_HANDLE_VALUE;
    }
}

bool KernelMouseRelay::Send(const MOUSE_INPUT_REPORT& report)
{
    std::lock_guard<std::mutex> lock(mutex_);

    if (hDevice_ == INVALID_HANDLE_VALUE) {
        return false;
    }

    MOUSE_INPUT_REPORT local = report;
    local.reportId = 1;

    DWORD bytesReturned = 0;
    BOOL ok = DeviceIoControl(
        hDevice_,
        IOCTL_INJECT_MOUSE,
        &local,
        static_cast<DWORD>(sizeof(local)),
        nullptr,
        0,
        &bytesReturned,
        nullptr);

    return ok != FALSE;
}
