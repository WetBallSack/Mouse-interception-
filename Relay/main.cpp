#include <windows.h>
#include <cstdio>
#include <atomic>

#include "KernelMouseRelay.h"
#include "SendInputDetour.h"
#include "MouseHook.h"

namespace {

std::atomic<bool> g_shouldExit{false};

BOOL WINAPI ConsoleCtrlHandler(DWORD type)
{
    switch (type) {
    case CTRL_C_EVENT:
    case CTRL_BREAK_EVENT:
    case CTRL_CLOSE_EVENT:
    case CTRL_LOGOFF_EVENT:
    case CTRL_SHUTDOWN_EVENT:
        g_shouldExit.store(true, std::memory_order_release);
        return TRUE;
    default:
        return FALSE;
    }
}

}

int wmain(int argc, wchar_t** /*argv*/)
{
    (void)argc;

    SetConsoleCtrlHandler(&ConsoleCtrlHandler, TRUE);

    KernelMouseRelay relay;
    if (!relay.IsOpen()) {
        std::fwprintf(stderr,
            L"[Relay] Failed to open %s (err=%lu). "
            L"Is VirtualMouse.sys installed and running?\n",
            VIRTUAL_MOUSE_USERMODE_PATH,
            GetLastError());
        return 1;
    }

    if (!SendInputDetour::Install(&relay)) {
        std::fwprintf(stderr, L"[Relay] MinHook detour install failed.\n");
        return 2;
    }

    MouseHook hook;
    if (!hook.Start()) {
        std::fwprintf(stderr,
            L"[Relay] WH_MOUSE_LL hook install failed (err=%lu).\n",
            GetLastError());
        SendInputDetour::Uninstall();
        return 3;
    }

    std::fwprintf(stdout,
        L"[Relay] Active. SendInput mouse events route through VirtualMouse.sys. "
        L"Ctrl+C to exit.\n");

    while (!g_shouldExit.load(std::memory_order_acquire)) {
        Sleep(100);
    }

    SendInputDetour::Uninstall();
    hook.Stop();
    return 0;
}
