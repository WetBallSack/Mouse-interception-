#pragma once

#include <windows.h>
#include <mutex>

extern "C" {
#include "driver_shared.h"
}

class KernelMouseRelay {
public:
    KernelMouseRelay();
    ~KernelMouseRelay();

    KernelMouseRelay(const KernelMouseRelay&)            = delete;
    KernelMouseRelay& operator=(const KernelMouseRelay&) = delete;

    bool IsOpen() const noexcept { return hDevice_ != INVALID_HANDLE_VALUE; }

    bool Send(const MOUSE_INPUT_REPORT& report);

private:
    HANDLE     hDevice_;
    std::mutex mutex_;
};
