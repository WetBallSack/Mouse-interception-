# Mouse Interception (VirtualMouse + Relay)

Production-style Windows accessibility / input-virtualisation middleware
that intercepts mouse input injected via `SendInput` and re-routes it
through a **KMDF + Virtual HID Framework (VHF)** kernel driver so the
events appear at the OS input stack as real HID-mouse reports (no
`LLMHF_INJECTED` flag).

```
App -> SendInput  --MinHook detour-->  KernelMouseRelay  --IOCTL-->  VirtualMouse.sys
                                                                        |
                                                                        +-- VHF pull
                                                                        |
                                                                        v
                                                              HID class driver -> OS input
WH_MOUSE_LL  --suppression-only safety net for legacy mouse_event paths-->
```

## Repository layout

```
VirtualMouseSolution.sln       Visual Studio 2022 solution
shared/
  driver_shared.h              MOUSE_INPUT_REPORT + IOCTL_INJECT_MOUSE (C/C++)
VirtualMouse/                  KMDF + VHF driver (C)
  driver.h
  driver.c                     DriverEntry, EvtDeviceAdd, VHF callback
  queue.c                      IOCTL handler + ring buffer
  VirtualMouse.inf             HIDClass INF (Root\VirtualMouse)
  VirtualMouse.vcxproj
Relay/                         Userspace relay (C++17, MSVC)
  KernelMouseRelay.{h,cpp}     RAII handle + DeviceIoControl wrapper
  SendInputDetour.{h,cpp}      MinHook detour on user32!SendInput
  MouseHook.{h,cpp}            WH_MOUSE_LL safety-net hook
  main.cpp                     Startup / shutdown
  Relay.vcxproj
RelayDll/                      Per-process payload DLL (C++17, MSVC)
  dllmain.cpp                  Spawns worker, opens driver, installs detour
  RelayDll.vcxproj             (reuses Relay/KernelMouseRelay.cpp + SendInputDetour.cpp)
Injector/                      Standalone injector (C++17, MSVC)
  Injector.cpp                 inject <pid> | launch <exe> [args]
  Injector.vcxproj
third_party/minhook/           (place MinHook headers + minhook.x64.lib here)
```

## Prerequisites

- Visual Studio 2022 with the **Desktop development with C++** workload.
- Windows 11 SDK (10.0.22621+).
- Windows Driver Kit (WDK) matching the SDK, including the WDK Visual
  Studio extension.
- KMDF 1.15 or newer (ships with the WDK).
- [MinHook](https://github.com/TsudaKageyu/minhook) prebuilt or built
  from source. Place the library so the layout is:
  ```
  third_party/minhook/include/MinHook.h
  third_party/minhook/lib/minhook.x64.lib
  ```
- Target machine: Windows 10 RS4+ / Windows 11 (x64), `vhf.sys` present
  (built into the OS).

## Building

Open `VirtualMouseSolution.sln` in Visual Studio 2022 and build the
`x64 / Release` configuration. Outputs land under `build/Release/x64/`.

- `VirtualMouse/`: `VirtualMouse.sys`, `VirtualMouse.inf`,
  `VirtualMouse.cat`.
- `Relay/`: `relay.exe` (standalone host + WH_MOUSE_LL global safety net).
- `RelayDll/`: `relay.dll` (per-process payload that opens the driver and
  installs the `SendInput` detour inside whichever process loads it).
- `Injector/`: `injector.exe` (loads `relay.dll` into existing processes
  by PID or into freshly-launched processes via `CREATE_SUSPENDED`).

## Installing the driver (test machine)

The target machine must be in **test-signing mode** unless the driver
is WHQL-signed:

```cmd
bcdedit /set testsigning on
shutdown /r /t 0
```

Test-sign the driver and INF (one-time on the dev box):

```cmd
makecert -r -pe -ss PrivateCertStore -n "CN=VirtualMouseTestCert" VirtualMouseTestCert.cer
certmgr.exe -add VirtualMouseTestCert.cer -s -r localMachine root
certmgr.exe -add VirtualMouseTestCert.cer -s -r localMachine trustedpublisher
signtool sign /v /s PrivateCertStore /n VirtualMouseTestCert /t http://timestamp.digicert.com VirtualMouse.sys
inf2cat /driver:. /os:10_x64
signtool sign /v /s PrivateCertStore /n VirtualMouseTestCert /t http://timestamp.digicert.com VirtualMouse.cat
```

Install with `devcon` (from WDK tools):

```cmd
devcon install VirtualMouse.inf Root\VirtualMouse
```

Uninstall:

```cmd
devcon remove Root\VirtualMouse
```

## Running

There are two roles:

1. **`relay.exe` — global controller.** Opens `\\.\VirtualMouse`, installs
   the per-process MinHook detour on `user32!SendInput` (for its own
   process), and starts the system-wide `WH_MOUSE_LL` safety net hook
   on a dedicated time-critical thread. `Ctrl+C` cleanly tears it down.

   ```cmd
   relay.exe
   ```

2. **`relay.dll` + `injector.exe` — covering arbitrary other processes.**
   `relay.dll` contains exactly the per-process payload (open driver +
   install `SendInput` detour). `injector.exe` loads it into a target
   process. Place `relay.dll` next to `injector.exe` (or pass
   `--dll <path>`).

   Attach to an already-running process by PID:

   ```cmd
   injector.exe inject 1234
   ```

   Launch a fresh process with the payload pre-loaded (created
   `CREATE_SUSPENDED`, DLL injected, then resumed — no race window):

   ```cmd
   injector.exe launch "C:\Path\To\Target.exe" --some --args
   ```

   Override the DLL path:

   ```cmd
   injector.exe --dll C:\custom\relay.dll inject 1234
   ```

Once the DLL is loaded inside a target process, every `SendInput` call
in that process carrying `INPUT_MOUSE` entries is intercepted, converted
to a `MOUSE_INPUT_REPORT`, and routed through `VirtualMouse.sys`.
Non-mouse (`INPUT_KEYBOARD`, `INPUT_HARDWARE`) entries pass through to
the real `SendInput`. `MOUSEEVENTF_ABSOLUTE` events are skipped (the
VHF device is relative-only).

The injector and the target process must match bitness: `relay.dll` is
x64-only, so target processes must also be native x64 (the injector
refuses WOW64 targets up front). Injecting into a process you do not
own typically requires Administrator (or the `SeDebugPrivilege`).

## Notes / constraints

- The MinHook detour is a per-process API hook. It affects only the
  process `relay.exe` is loaded into. To cover other processes, build
  `relay.exe` as a DLL and load it via your preferred injection
  mechanism.
- `MOUSEEVENTF_ABSOLUTE` events are skipped: the VHF descriptor is
  relative-only. Convert to relative deltas before calling `SendInput`
  if you need routing.
- `MOUSE_INPUT_REPORT.reportId` is forced to `1` on every IOCTL so the
  HID class driver always sees a descriptor-matching prefix.
- `#pragma pack(push, 1)` on `MOUSE_INPUT_REPORT` is mandatory; do not
  add fields without updating the HID descriptor in `driver.c`.
- The INF `ClassGuid` is `{745a17a0-74d3-11d0-b6fe-00a0c90f57da}`
  (HIDClass); changing it will break HID class binding.
