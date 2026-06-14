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

## End-to-end walkthrough: getting it running on `program.exe`

This is the full path from a clean Windows 11 x64 machine to
`program.exe` having its `SendInput` mouse calls re-emitted as real
HID-mouse reports by the kernel driver. Steps that have to be done
exactly once on a machine are marked **(one-time)**; the rest are
per-session.

### 0. Pick your machines

You can do everything on a single machine, but the recommended layout
is two machines (or two VMs):

- **Dev box**: where you build and code-sign. Anything reasonable.
- **Target box**: where the driver is installed and `program.exe`
  runs. Windows 10 RS4+ or Windows 11, x64.

If you only have one machine, just play both roles on it.

### 1. **(one-time, dev box)** Install build tools

1. Install **Visual Studio 2022** with the *Desktop development with
   C++* workload.
2. Install the **Windows 11 SDK** (10.0.22621 or newer).
3. Install the matching **Windows Driver Kit (WDK)** and the WDK Visual
   Studio extension.
4. Get **MinHook** (https://github.com/TsudaKageyu/minhook), build the
   `x64 / Release` configuration, and drop the outputs in:
   ```
   third_party/minhook/include/MinHook.h
   third_party/minhook/lib/minhook.x64.lib
   ```

### 2. **(per build)** Build everything

Open `VirtualMouseSolution.sln`, select `Release | x64`, and build.
You'll end up with the following under `build\Release\x64\`:

```
VirtualMouse\VirtualMouse.sys
VirtualMouse\VirtualMouse.inf
VirtualMouse\VirtualMouse.cat   (after inf2cat below)
Relay\relay.exe
RelayDll\relay.dll
Injector\injector.exe
```

Copy that whole folder to a working directory on the target machine,
e.g. `C:\VirtualMouse\`.

### 3. **(one-time, target box)** Enable test-signing and reboot

Unless you have a WHQL-signed driver, the target needs test-signing
on. Open an elevated `cmd`:

```cmd
bcdedit /set testsigning on
shutdown /r /t 0
```

After the reboot, the desktop watermark in the bottom-right corner
will say "Test Mode".

### 4. **(one-time, dev box)** Generate a test cert and sign the driver

From a *Developer Command Prompt for VS 2022*, in the folder that
contains `VirtualMouse.sys` + `VirtualMouse.inf`:

```cmd
makecert -r -pe -ss PrivateCertStore -n "CN=VirtualMouseTestCert" VirtualMouseTestCert.cer
signtool sign /v /s PrivateCertStore /n VirtualMouseTestCert /fd sha256 ^
    /t http://timestamp.digicert.com VirtualMouse.sys
inf2cat /driver:. /os:10_x64
signtool sign /v /s PrivateCertStore /n VirtualMouseTestCert /fd sha256 ^
    /t http://timestamp.digicert.com VirtualMouse.cat
```

Copy `VirtualMouseTestCert.cer` to the target machine alongside the
other files.

### 5. **(one-time, target box)** Trust the test cert and install the driver

Elevated `cmd` in your working directory (e.g. `C:\VirtualMouse\`):

```cmd
certutil -addstore -f Root VirtualMouseTestCert.cer
certutil -addstore -f TrustedPublisher VirtualMouseTestCert.cer

devcon install VirtualMouse.inf Root\VirtualMouse
```

`devcon` ships with the WDK; if you don't have it on the target, you
can install via *Device Manager → Action → Add legacy hardware → Have
Disk* and point it at `VirtualMouse.inf`.

Confirm it's running:

```cmd
sc query VirtualMouse
```

You should see `STATE : 4 RUNNING`. Open Device Manager → *Mice and
other pointing devices*; you should see a new entry called
*VirtualMouse VHF Relative Mouse*.

To uninstall later: `devcon remove Root\VirtualMouse`.

### 6. **(per session, target box)** Start the global controller

Open an elevated `cmd` in your working directory:

```cmd
relay.exe
```

You'll see:

```
[Relay] Active. SendInput mouse events route through VirtualMouse.sys.
Ctrl+C to exit.
```

Leave this window open. It owns the system-wide `WH_MOUSE_LL`
suppression hook (which silently swallows any legacy `mouse_event`
calls that still leak `LLMHF_INJECTED`).

### 7. **(per session, target box)** Get the payload into `program.exe`

You have two options. Pick whichever fits your scenario.

**Option A — launch `program.exe` with the payload pre-loaded** (best;
no race between the program starting and the hook installing):

```cmd
injector.exe launch "C:\Path\To\program.exe" --any --args --it --needs
```

You'll see `[Injector] Launched ... with relay.dll injected.` and
`program.exe` will start up normally. Every mouse `SendInput` it makes
from then on goes through the kernel driver.

**Option B — attach to a `program.exe` that's already running**:

```cmd
tasklist | findstr /i program.exe
injector.exe inject 12345
```

(Replace `12345` with the PID printed by `tasklist`.) You'll see
`[Injector] Injected ... into pid 12345.` From that moment on,
`program.exe`'s `SendInput` calls are routed.

Both options need an elevated prompt if `program.exe` runs as a
different user or as Administrator. `program.exe` itself must be x64
(WOW64/32-bit targets are rejected up front — `relay.dll` is x64).

### 8. **(verify it's working)** Confirm events are flowing

Quickest sanity check: open an elevated PowerShell on the target box
and run a one-liner that calls `SendInput` from inside a host you
inject:

```cmd
injector.exe launch "C:\Windows\System32\notepad.exe"
```

Then, from another tool / script running inside that injected
`notepad.exe`, fire `SendInput`s and watch the cursor move. (Notepad
itself doesn't call `SendInput`; you'd use it as a host for a small
test harness or scripting engine you control.)

For a deeper check, install Microsoft's *HID Test Utility* or run
`hidapitester` / `HClient` (from the WDK samples) and verify that
incoming reports show up on the *VirtualMouse VHF Relative Mouse* HID
node. You can also attach WinDbg with `!devnode 0 1 HID` to inspect
the device tree.

### 9. **(per session, when you're done)** Shut down cleanly

1. Close `program.exe` (or call `FreeLibrary(relay.dll)` from inside
   it if your tooling supports it — `DllMain` tears the detour down
   cleanly on `DLL_PROCESS_DETACH`).
2. `Ctrl+C` in the `relay.exe` window.
3. Driver stays installed; reboots are not required between sessions.

### Troubleshooting

| Symptom | Likely cause |
|---|---|
| `relay.exe` says "Failed to open \\.\VirtualMouse" | Driver not installed, not started, or you forgot to launch the prompt elevated. Check `sc query VirtualMouse`. |
| `injector.exe` says "LoadLibraryW in target returned NULL" | The target couldn't load `relay.dll` — typically a missing MSVC runtime, bitness mismatch, or `relay.dll` path issue. Make sure `relay.dll` is next to `injector.exe` (or pass `--dll`). |
| `injector.exe` says "Target process bitness does not match" | The target is 32-bit (WOW64). This build is x64-only. |
| `devcon install` fails with "driver is not signed" | Test-signing isn't on, or you skipped the `certutil -addstore TrustedPublisher` step. |
| Cursor doesn't move when `program.exe` calls `SendInput` | Check that the injection succeeded (Process Hacker → modules tab → look for `relay.dll`). Also confirm the calls aren't using `MOUSEEVENTF_ABSOLUTE` (skipped by design). |

## Notes / constraints

- The MinHook detour is a per-process API hook. `relay.exe` covers
  only itself; `relay.dll` (loaded via `injector.exe`) covers each
  process it's injected into.
- `MOUSEEVENTF_ABSOLUTE` events are skipped: the VHF descriptor is
  relative-only. Convert to relative deltas before calling `SendInput`
  if you need routing.
- `MOUSE_INPUT_REPORT.reportId` is forced to `1` on every IOCTL so the
  HID class driver always sees a descriptor-matching prefix.
- `#pragma pack(push, 1)` on `MOUSE_INPUT_REPORT` is mandatory; do not
  add fields without updating the HID descriptor in `driver.c`.
- The INF `ClassGuid` is `{745a17a0-74d3-11d0-b6fe-00a0c90f57da}`
  (HIDClass); changing it will break HID class binding.
