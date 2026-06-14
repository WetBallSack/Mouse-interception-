#pragma once

#include <windows.h>

class MouseHook {
public:
    MouseHook();
    ~MouseHook();

    MouseHook(const MouseHook&)            = delete;
    MouseHook& operator=(const MouseHook&) = delete;

    bool Start();
    void Stop();

private:
    static DWORD WINAPI ThreadProc(LPVOID param);
    static LRESULT CALLBACK LowLevelMouseProc(int code, WPARAM wParam, LPARAM lParam);

    HANDLE       thread_;
    HANDLE       readyEvent_;
    DWORD        threadId_;
    bool         installOk_;
    static HHOOK s_hook_;
};
