# Windows 10/11 x64

Quartz has native Windows backends for USB, keyboard input, audio capture, media
artwork, process memory, hardware breakpoints, register capture and assembly.
WSL is not needed. Use Windows 10 version 1903 or later, or Windows 11, on x64,
with an OpenGL 3.3 capable graphics driver. Windows on ARM and MSVC builds are
not currently supported.

## Run

Extract the entire portable folder and run `quartz.exe`. Keep its DLLs and
`tools` directory together. The assembler tools are required by the RE patch
editor. Preferences, scripts and shader files live in `%APPDATA%\Quartz`.
`QUARTZ_CONFIG_HOME` can override this directory for isolated workspaces.
Closing the window hides it; Ctrl+Alt+Shift+Q restores it. Use the application's
exit command to shut down completely.

To start hidden when signing in, run
`scripts/install-windows-startup.ps1 -Executable build/Quartz-Windows-x64/quartz.exe`
from the repository root. The script creates a shortcut in your user Startup
folder; it does not require administrator rights.

## Quartz USB driver (once per device)

The composite keyboard has two different interfaces. Interface 00 is the HID
keyboard; interface 01 is the vendor RPC channel. Only interface 01 needs WinUSB.

Connect the keyboard and run `quartz-winusb-setup.exe` to inspect the device and
driver selection without changing anything. To install, open PowerShell in the
portable folder and run:

```powershell
Start-Process .\quartz-winusb-setup.exe -ArgumentList '--install' -Verb RunAs -Wait
```

Approve Windows' administrator prompt. The helper binds Microsoft's signed,
built-in WinUSB driver exclusively to `USB\VID_B147&PID_4131&MI_01`. It does not
replace the HID driver or change driver-signing policy. See `driver-install.log`
beside the helper for results. Reconnect the keyboard if requested. Installation
requires administrator rights; normal Quartz USB use does not.

If a previous tool installed a different driver, review the inspection output
before replacing it. The helper supports only the Quartz B147:4131 product.

## Audio, media and input

The default audio source captures the current output using WASAPI loopback.
The source list also includes individual outputs and microphone/input endpoints.
Input endpoints are subject to Windows microphone permissions.
Media title and artwork come from Windows System Media Transport Controls;
the player must publish a Windows media session and artwork.

Raw Input provides global physical-key events from the Quartz keyboard, even
when Quartz is unfocused. GLFW provides the focused-window fallback. Existing
script/profile key IDs retain their Linux numeric values on both platforms.

## Reverse engineering

Both x64 and WOW64/x86 targets are supported. Windows access permissions apply:
run Quartz as administrator when inspecting an elevated target. Protected
processes can reject access. Windows allows one debugger to attach to a process
at a time, so close another debugger before using an execution probe or hardware
access monitor. Memory scanning and ordinary reads do not attach a debugger.

Hardware monitoring handles existing and newly created target threads, restores
the borrowed debug-register slots on cleanup, and detaches without terminating
the target. As on Linux, monitors require appropriately aligned hardware
breakpoint addresses and supported sizes.

## Build

Install [MSYS2](https://www.msys2.org/) in `C:\msys64`. In the UCRT64 terminal:

```sh
pacman -S --needed mingw-w64-ucrt-x86_64-gcc mingw-w64-ucrt-x86_64-cmake mingw-w64-ucrt-x86_64-ninja mingw-w64-ucrt-x86_64-pkgconf
# Optional: tests against a real 32-bit target
pacman -S --needed mingw-w64-i686-gcc
```

Initialize repository submodules (`git submodule update --init --recursive`).
Then from PowerShell at the repository root:

```powershell
.\scripts\build-windows.ps1 -Test
```

This builds, tests and packages `build\Quartz-Windows-x64` and a ZIP beside it.
The portable folder includes the required runtime DLLs and GNU assembler tools.
Third-party license files are included under `licenses`. GNU binutils and runtime
source packages are available from [MSYS2 package sources](https://github.com/msys2/MINGW-packages)
and [MSYS2 source archives](https://repo.msys2.org/mingw/sources/); retain the
corresponding source and license obligations when redistributing binaries.

## Validation

`ctest --test-dir build/windows --output-on-failure` checks QuickJS, assembly,
disassembly, memory protection restoration, process discovery, reads/writes,
pointer resolution, scans, hardware monitors, execution probes and cleanup.
It uses only disposable fixture processes launched by the test. Installing the
i686 compiler also enables the x86 fixture.

Run `build/windows/quartz-windows-tests.exe --desktop-only` in a desktop session
to additionally compile/render every built-in shader and check real WASAPI,
media-session, Raw Input and Quartz USB communication. This requires the keyboard
and WinUSB driver. These hardware checks are deliberately separate from CI.
Use `--host-only` instead to check graphics, audio, media and keyboard input
without opening the RPC interface while another Quartz instance owns it.

The `rms-shader-switch` CTest checks the example script through Quartz's actual
QuickJS SDK, including threshold edits, persistent properties, previous-shader
selection and mutex ownership/release. See [script setup](../scripts/README.md)
in the source checkout for its controls.
