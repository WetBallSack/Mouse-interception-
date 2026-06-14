#include "MouseHook.h"

HHOOK MouseHook::s_hook_ = nullptr;

MouseHook::MouseHook()
    : thread_(nullptr)
    , readyEvent_(nullptr)
    , threadId_(0)
    , installOk_(false)
{
}

MouseHook::~MouseHook()
{
    Stop();
}

LRESULT CALLBACK MouseHook::LowLevelMouseProc(int code, WPARAM wParam, LPARAM lParam)
{
    if (code == HC_ACTION) {
        const MSLLHOOKSTRUCT* p = reinterpret_cast<const MSLLHOOKSTRUCT*>(lParam);
        if (p && (p->flags & LLMHF_INJECTED)) {
            return 1;
        }
    }
    return CallNextHookEx(s_hook_, code, wParam, lParam);
}

DWORD WINAPI MouseHook::ThreadProc(LPVOID param)
{
    MouseHook* self = static_cast<MouseHook*>(param);

    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);

    s_hook_ = SetWindowsHookExW(
        WH_MOUSE_LL,
        &MouseHook::LowLevelMouseProc,
        GetModuleHandleW(nullptr),
        0);

    self->installOk_ = (s_hook_ != nullptr);
    SetEvent(self->readyEvent_);

    if (!self->installOk_) {
        return 1;
    }

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    if (s_hook_ != nullptr) {
        UnhookWindowsHookEx(s_hook_);
        s_hook_ = nullptr;
    }
    return 0;
}

bool MouseHook::Start()
{
    if (thread_ != nullptr) {
        return installOk_;
    }

    readyEvent_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (readyEvent_ == nullptr) {
        return false;
    }

    thread_ = CreateThread(nullptr, 0, &MouseHook::ThreadProc, this, 0, &threadId_);
    if (thread_ == nullptr) {
        CloseHandle(readyEvent_);
        readyEvent_ = nullptr;
        return false;
    }

    WaitForSingleObject(readyEvent_, INFINITE);
    CloseHandle(readyEvent_);
    readyEvent_ = nullptr;

    return installOk_;
}

void MouseHook::Stop()
{
    if (thread_ == nullptr) {
        return;
    }

    if (threadId_ != 0) {
        PostThreadMessageW(threadId_, WM_QUIT, 0, 0);
    }

    WaitForSingleObject(thread_, 5000);
    CloseHandle(thread_);
    thread_    = nullptr;
    threadId_  = 0;
    installOk_ = false;
}
