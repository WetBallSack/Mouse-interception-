#include "SendInputDetour.h"
#include "KernelMouseRelay.h"

#include <MinHook.h>

#include <atomic>
#include <vector>
#include <cstdio>

namespace {

using SendInput_t = UINT(WINAPI*)(UINT, LPINPUT, int);

SendInput_t                       g_origSendInput = nullptr;
std::atomic<KernelMouseRelay*>    g_relay{nullptr};
std::atomic<bool>                 g_installed{false};

UCHAR ButtonsFromFlags(DWORD flags, UCHAR current)
{
    UCHAR out = current;

    if (flags & MOUSEEVENTF_LEFTDOWN)   out |= VMOUSE_BTN_LEFT;
    if (flags & MOUSEEVENTF_LEFTUP)     out &= static_cast<UCHAR>(~VMOUSE_BTN_LEFT);
    if (flags & MOUSEEVENTF_RIGHTDOWN)  out |= VMOUSE_BTN_RIGHT;
    if (flags & MOUSEEVENTF_RIGHTUP)    out &= static_cast<UCHAR>(~VMOUSE_BTN_RIGHT);
    if (flags & MOUSEEVENTF_MIDDLEDOWN) out |= VMOUSE_BTN_MIDDLE;
    if (flags & MOUSEEVENTF_MIDDLEUP)   out &= static_cast<UCHAR>(~VMOUSE_BTN_MIDDLE);

    return out;
}

SHORT ClampToShort(LONG v)
{
    if (v >  32767) return  32767;
    if (v < -32768) return -32768;
    return static_cast<SHORT>(v);
}

CHAR WheelFromMouseData(DWORD mouseData)
{
    SHORT raw = static_cast<SHORT>(HIWORD(mouseData));
    LONG ticks = raw / WHEEL_DELTA;
    if (ticks >  127) ticks =  127;
    if (ticks < -128) ticks = -128;
    return static_cast<CHAR>(ticks);
}

thread_local UCHAR t_buttonState = 0;

UINT WINAPI DetourSendInput(UINT cInputs, LPINPUT pInputs, int cbSize)
{
    if (cInputs == 0 || pInputs == nullptr || cbSize != sizeof(INPUT)) {
        return g_origSendInput
            ? g_origSendInput(cInputs, pInputs, cbSize)
            : 0;
    }

    KernelMouseRelay* relay = g_relay.load(std::memory_order_acquire);

    std::vector<INPUT> passthrough;
    passthrough.reserve(cInputs);

    UINT routed = 0;

    for (UINT i = 0; i < cInputs; ++i) {
        const INPUT& in = pInputs[i];

        if (in.type != INPUT_MOUSE) {
            passthrough.push_back(in);
            continue;
        }

        const MOUSEINPUT& m = in.mi;

        if (m.dwFlags & MOUSEEVENTF_ABSOLUTE) {
            std::fprintf(stderr,
                "[Relay] WARN: MOUSEEVENTF_ABSOLUTE event skipped "
                "(VHF device is relative-only).\n");
            ++routed;
            continue;
        }

        MOUSE_INPUT_REPORT rep{};
        rep.reportId = 1;
        rep.x        = (m.dwFlags & MOUSEEVENTF_MOVE) ? ClampToShort(m.dx) : 0;
        rep.y        = (m.dwFlags & MOUSEEVENTF_MOVE) ? ClampToShort(m.dy) : 0;
        rep.wheel    = (m.dwFlags & MOUSEEVENTF_WHEEL)
                            ? WheelFromMouseData(m.mouseData)
                            : 0;

        t_buttonState = ButtonsFromFlags(m.dwFlags, t_buttonState);
        rep.buttons   = t_buttonState;

        if (relay != nullptr && relay->IsOpen()) {
            relay->Send(rep);
        }

        ++routed;
    }

    if (!passthrough.empty() && g_origSendInput != nullptr) {
        UINT n = g_origSendInput(
            static_cast<UINT>(passthrough.size()),
            passthrough.data(),
            cbSize);
        return n + routed;
    }

    return routed;
}

} // namespace

namespace SendInputDetour {

bool Install(KernelMouseRelay* relay)
{
    if (g_installed.load(std::memory_order_acquire)) {
        g_relay.store(relay, std::memory_order_release);
        return true;
    }

    if (MH_Initialize() != MH_OK) {
        return false;
    }

    MH_STATUS s = MH_CreateHookApi(
        L"user32",
        "SendInput",
        reinterpret_cast<LPVOID>(&DetourSendInput),
        reinterpret_cast<LPVOID*>(&g_origSendInput));

    if (s != MH_OK) {
        MH_Uninitialize();
        return false;
    }

    if (MH_EnableHook(MH_ALL_HOOKS) != MH_OK) {
        MH_Uninitialize();
        g_origSendInput = nullptr;
        return false;
    }

    g_relay.store(relay, std::memory_order_release);
    g_installed.store(true, std::memory_order_release);
    return true;
}

void Uninstall()
{
    if (!g_installed.exchange(false, std::memory_order_acq_rel)) {
        return;
    }
    MH_DisableHook(MH_ALL_HOOKS);
    MH_Uninitialize();
    g_origSendInput = nullptr;
    g_relay.store(nullptr, std::memory_order_release);
}

} // namespace SendInputDetour
