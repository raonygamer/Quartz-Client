# Quartz Client

A native Linux configuration and control application for Quartz-powered devices.

## Architecture

The client is split into explicit subsystems under `include/quartz/client` and `src`:

- `Application` owns top-level application orchestration and the main update loop.
- `platform` owns the GLFW/OpenGL window lifecycle and third-party implementation translation units.
- `ui` owns ImGui lifecycle, `PageManager`, and individual `Page` implementations.
- `usb` separates raw libusb transport (`USBTransport`) from QRPC packet framing/session handling (`QRPCSession`). `RawUSB` remains as the application-facing compatibility facade.
- `runtime` owns runtime bindings, controls, profiles, telemetry, derived state, and object/pointer models.
- `native` owns native-process/signature/register-capture types; native implementation lives in `src/runtime/RuntimeNative.cpp`.
- `shader` owns shader sources/presets, reflected material state, framebuffer rendering, transitions, and editor state.
- `input`, `audio`, `media`, `device`, and `settings` contain their corresponding application services and models.

`src/Main.cpp` is intentionally only the process entry point; real application startup and orchestration lives in `Application`.

## Building

Quartz Client uses CMake and C++20. Clone recursively so the protocol and vendor submodules are available, then configure and build:

```bash
git clone --recursive https://github.com/raonygamer/Quartz-Client.git
cd Quartz-Client
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
```

The executable is emitted as `quartz`.

## Project

Quartz Client communicates with Quartz firmware over QRPC and can drive the keyboard framebuffer, inspect device performance/timing information, render shader-based RGB effects, analyze audio/input/media state, and feed its runtime binding/control system from native process state.

Made by Raony Reis. Not affiliated with Redragon.
