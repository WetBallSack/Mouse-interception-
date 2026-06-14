#include <windows.h>
#include <atomic>

#include "KernelMouseRelay.h"
#include "SendInputDetour.h"

namespace {

std::atomic<bool>   g_active{false};
KernelMouseRelay*   g_relay = nullptr;

DWORD WINAPI WorkerInit(LPVOID)
{
    g_relay = new (std::nothrow) KernelMouseRelay();
    if (g_relay == nullptr || !g_relay->IsOpen()) {
        return 1;
    }
    if (!SendInputDetour::Install(g_relay)) {
        return 2;
    }
    return 0;
}

void Teardown()
{
    if (!g_active.exchange(false, std::memory_order_acq_rel)) {
        return;
    }
    SendInputDetour::Uninstall();
    delete g_relay;
    g_relay = nullptr;
}

}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID reserved)
{
    switch (reason) {
    case DLL_PROCESS_ATTACH: {
        DisableThreadLibraryCalls(hModule);
        bool expected = false;
        if (!g_active.compare_exchange_strong(expected, true)) {
            return TRUE;
        }
        HANDLE h = CreateThread(nullptr, 0, &WorkerInit, nullptr, 0, nullptr);
        if (h != nullptr) {
            CloseHandle(h);
        } else {
            g_active.store(false, std::memory_order_release);
            return FALSE;
        }
        break;
    }
    case DLL_PROCESS_DETACH:
        if (reserved == nullptr) {
            Teardown();
        }
        break;
    default:
        break;
    }
    return TRUE;
}
