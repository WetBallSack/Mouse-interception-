#include <windows.h>
#include <shlwapi.h>
#include <cstdio>
#include <string>
#include <vector>

#pragma comment(lib, "shlwapi.lib")

namespace {

void PrintUsage()
{
    std::fwprintf(stderr,
        L"Usage:\n"
        L"  injector.exe [--dll <path>] inject <pid>\n"
        L"  injector.exe [--dll <path>] launch <exe> [args...]\n"
        L"\n"
        L"Default --dll is <injector-dir>\\relay.dll\n");
}

std::wstring DefaultDllPath()
{
    wchar_t buf[MAX_PATH] = {};
    DWORD n = GetModuleFileNameW(nullptr, buf, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) {
        return L"relay.dll";
    }
    PathRemoveFileSpecW(buf);
    std::wstring p = buf;
    p += L"\\relay.dll";
    return p;
}

bool ProcessBitnessMatches(HANDLE hProcess)
{
    USHORT processMachine = 0;
    USHORT nativeMachine  = 0;
    if (!IsWow64Process2(hProcess, &processMachine, &nativeMachine)) {
        return true;
    }
    if (processMachine == IMAGE_FILE_MACHINE_UNKNOWN) {
        return nativeMachine == IMAGE_FILE_MACHINE_AMD64
            || nativeMachine == IMAGE_FILE_MACHINE_ARM64;
    }
    return false;
}

bool InjectDll(HANDLE hProcess, const std::wstring& dllPath)
{
    if (!ProcessBitnessMatches(hProcess)) {
        std::fwprintf(stderr,
            L"[Injector] Target process bitness does not match relay.dll "
            L"(relay.dll is x64; target is WOW64/x86).\n");
        return false;
    }

    SIZE_T bytes = (dllPath.size() + 1) * sizeof(wchar_t);

    LPVOID remote = VirtualAllocEx(
        hProcess, nullptr, bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (remote == nullptr) {
        std::fwprintf(stderr,
            L"[Injector] VirtualAllocEx failed (err=%lu).\n", GetLastError());
        return false;
    }

    SIZE_T written = 0;
    if (!WriteProcessMemory(hProcess, remote, dllPath.c_str(), bytes, &written)
        || written != bytes) {
        std::fwprintf(stderr,
            L"[Injector] WriteProcessMemory failed (err=%lu).\n",
            GetLastError());
        VirtualFreeEx(hProcess, remote, 0, MEM_RELEASE);
        return false;
    }

    HMODULE k32 = GetModuleHandleW(L"kernel32.dll");
    if (k32 == nullptr) {
        VirtualFreeEx(hProcess, remote, 0, MEM_RELEASE);
        return false;
    }

    LPTHREAD_START_ROUTINE loadLibraryW =
        reinterpret_cast<LPTHREAD_START_ROUTINE>(
            GetProcAddress(k32, "LoadLibraryW"));
    if (loadLibraryW == nullptr) {
        VirtualFreeEx(hProcess, remote, 0, MEM_RELEASE);
        return false;
    }

    HANDLE hThread = CreateRemoteThread(
        hProcess, nullptr, 0, loadLibraryW, remote, 0, nullptr);
    if (hThread == nullptr) {
        std::fwprintf(stderr,
            L"[Injector] CreateRemoteThread failed (err=%lu).\n",
            GetLastError());
        VirtualFreeEx(hProcess, remote, 0, MEM_RELEASE);
        return false;
    }

    WaitForSingleObject(hThread, INFINITE);

    DWORD exitCode = 0;
    GetExitCodeThread(hThread, &exitCode);
    CloseHandle(hThread);
    VirtualFreeEx(hProcess, remote, 0, MEM_RELEASE);

    if (exitCode == 0) {
        std::fwprintf(stderr,
            L"[Injector] LoadLibraryW in target returned NULL.\n");
        return false;
    }
    return true;
}

int CmdInject(DWORD pid, const std::wstring& dllPath)
{
    HANDLE hProcess = OpenProcess(
        PROCESS_CREATE_THREAD | PROCESS_VM_OPERATION | PROCESS_VM_WRITE
            | PROCESS_VM_READ | PROCESS_QUERY_INFORMATION,
        FALSE,
        pid);

    if (hProcess == nullptr) {
        std::fwprintf(stderr,
            L"[Injector] OpenProcess(%lu) failed (err=%lu). "
            L"Run as Administrator?\n", pid, GetLastError());
        return 1;
    }

    bool ok = InjectDll(hProcess, dllPath);
    CloseHandle(hProcess);

    if (ok) {
        std::fwprintf(stdout,
            L"[Injector] Injected %s into pid %lu.\n",
            dllPath.c_str(), pid);
        return 0;
    }
    return 1;
}

std::wstring BuildCommandLine(int argc, wchar_t** argv, int startIdx)
{
    std::wstring cl;
    for (int i = startIdx; i < argc; ++i) {
        if (!cl.empty()) cl += L' ';
        std::wstring a = argv[i];
        bool needQuotes = a.empty() || a.find_first_of(L" \t\"") != std::wstring::npos;
        if (needQuotes) {
            cl += L'"';
            for (wchar_t c : a) {
                if (c == L'"') cl += L'\\';
                cl += c;
            }
            cl += L'"';
        } else {
            cl += a;
        }
    }
    return cl;
}

int CmdLaunch(int argc, wchar_t** argv, int exeIdx, const std::wstring& dllPath)
{
    std::wstring cl = BuildCommandLine(argc, argv, exeIdx);
    std::vector<wchar_t> mut(cl.begin(), cl.end());
    mut.push_back(L'\0');

    STARTUPINFOW si = {};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi = {};

    if (!CreateProcessW(
            argv[exeIdx],
            mut.data(),
            nullptr, nullptr, FALSE,
            CREATE_SUSPENDED,
            nullptr, nullptr,
            &si, &pi)) {
        std::fwprintf(stderr,
            L"[Injector] CreateProcessW(%s) failed (err=%lu).\n",
            argv[exeIdx], GetLastError());
        return 1;
    }

    bool ok = InjectDll(pi.hProcess, dllPath);
    if (!ok) {
        TerminateProcess(pi.hProcess, 1);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        return 1;
    }

    if (ResumeThread(pi.hThread) == (DWORD)-1) {
        std::fwprintf(stderr,
            L"[Injector] ResumeThread failed (err=%lu).\n", GetLastError());
        TerminateProcess(pi.hProcess, 1);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        return 1;
    }

    std::fwprintf(stdout,
        L"[Injector] Launched %s (pid=%lu) with %s injected.\n",
        argv[exeIdx], pi.dwProcessId, dllPath.c_str());

    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return 0;
}

}

int wmain(int argc, wchar_t** argv)
{
    if (argc < 3) {
        PrintUsage();
        return 2;
    }

    std::wstring dllPath = DefaultDllPath();
    int idx = 1;

    if (wcscmp(argv[idx], L"--dll") == 0) {
        if (argc < idx + 3) { PrintUsage(); return 2; }
        dllPath = argv[idx + 1];
        idx += 2;
    }

    if (GetFileAttributesW(dllPath.c_str()) == INVALID_FILE_ATTRIBUTES) {
        std::fwprintf(stderr,
            L"[Injector] DLL not found at %s\n", dllPath.c_str());
        return 2;
    }

    const wchar_t* cmd = argv[idx];

    if (wcscmp(cmd, L"inject") == 0) {
        if (argc < idx + 2) { PrintUsage(); return 2; }
        DWORD pid = wcstoul(argv[idx + 1], nullptr, 10);
        if (pid == 0) { PrintUsage(); return 2; }
        return CmdInject(pid, dllPath);
    }

    if (wcscmp(cmd, L"launch") == 0) {
        if (argc < idx + 2) { PrintUsage(); return 2; }
        return CmdLaunch(argc, argv, idx + 1, dllPath);
    }

    PrintUsage();
    return 2;
}
