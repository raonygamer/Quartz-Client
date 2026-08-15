# Quartz Client

> A native configuration, visualization, diagnostics and reverse-engineering companion for Quartz-powered devices.

Quartz Client is the desktop companion for my Quartz firmware projects. It talks directly to Quartz devices over the custom USB RPC interface, streams RGB framebuffers, exposes firmware diagnostics and provides the host-side tooling I use while developing the firmware and experimenting with native applications.

It started as a small RGB visualizer/client and **ended up being a minified Cheat Engine by fun**.

This is a **personal project written for my own devices and my own usage**. It is public because someone else might find it useful or interesting, but I do not intend to turn it into a general-purpose keyboard configuration utility or support arbitrary hardware. If support for another device ever appears, it will be because I personally wanted or needed it.

## Current status

Quartz Client is actively developed and already used alongside the Quartz K552X firmware. The codebase has moved well past the original monolithic `Main.cpp`: UI, USB/RPC, native-process tooling, runtime bindings, profiles, rendering, device state and platform integration now live in dedicated subsystems.

The application is written in modern **C++20** and uses **OpenGL**, **Dear ImGui**, raw **libusb**, **Zydis** and **libhat**.

## Features

### Quartz device communication

- Raw libusb device discovery and communication
- Shared packet definitions through `Quartz-Protocol`
- QRPC request/response transport and packet inspection
- Direct RGB framebuffer streaming
- Device selection and connection state
- Firmware performance telemetry
- Matrix timing probes
- USB traffic/profiling diagnostics

### RGB and shaders

- OpenGL shader-driven keyboard framebuffer rendering
- Live fragment/vertex shader editing
- Shader presets and materials
- Runtime uniforms sourced from keyboard state, lock indicators and external data
- Supersampled keyboard framebuffer with configurable downsampling
- Audio-reactive effects and spectrum visualization
- MPRIS media artwork colors and player selection
- Global brightness and base-color controls
- Live keyboard preview

### Runtime bindings and automation

Quartz exposes host/device/application state through a runtime binding graph rather than hard-coding every experiment into the renderer.

- Native-process values and addresses
- Signature-resolved addresses
- Binding-to-binding sources
- Object fields and object status
- Aggregates and mass comparisons
- Value bank
- Shader/render state
- Device/RPC/USB telemetry
- Keyboard and audio state
- Controls with conditions, actions and nested grouping
- Profiles and profile hotkeys
- Runtime status/error feedback and rescan/rebind operations

### Reverse engineering workspace

The client includes a native-process workspace used for my own debugging and visualizer experiments.

- Cheat-Engine-style value scanner with next-scan filtering
- Decimal and `0x` hexadecimal numeric input
- Live visible-result refresh
- Previous-scan comparison with increase/decrease coloring
- Editable watch list with configurable types, addresses and values
- Freeze values at a configurable refresh rate
- Manual **Add to watch list** without running a scan first
- Whole-row context actions for inspection, disassembly, watches and bindings
- Hardware data breakpoints for read/write access discovery
- Grouped access sites with hit counters
- Register snapshots for hardware-watch hits
- One-shot hardware execution probes to capture registers at a specific instruction
- Architecture-aware x86/x64 disassembly using Zydis
- Syntax-colored Intel assembly view
- Persistent disassembly markers with tags and custom colors
- Red `armed for next execution` marker for one-shot probes
- Scrollable raw-byte view with synchronized addresses
- Raw bytes / Registers / Patch bytes lower inspector tabs
- Direct memory byte patching
- Fast asynchronous hexadecimal signature search using libhat
- Signature results can be inspected, disassembled, watched or turned into runtime bindings
- Reusable object-layout descriptors
- Live **Object model debugger** for binding a descriptor to an arbitrary process/base address
- Object fields can be pushed directly into the watch list, access watcher or runtime binding system
- Target-process pointer width is respected while debugging 32-bit object layouts from the 64-bit host

### Desktop integration

- KDE StatusNotifier tray integration
- Start/minimize-to-tray behavior
- Full ImGui application workspace
- Runtime/application CPU diagnostics

## Architecture

The original prototype grew inside a single translation unit. The client is now split into subsystem-focused headers and source files.

```text
Quartz Client
├── application / window
├── ui
│   ├── pages
│   ├── memory inspector
│   └── reverse-engineering tools
├── device state
├── usb
│   ├── raw libusb transport
│   └── QRPC session
├── native
│   ├── memory scanner
│   ├── hardware watchpoints
│   ├── execution probes
│   ├── signature scanner
│   └── disassembly / opcode tools
├── runtime
│   ├── bindings
│   ├── controls
│   ├── profiles
│   ├── value bank
│   └── object models / pointers
├── shader / rendering
├── audio / media
└── Quartz-Protocol
```

## Protocol

Quartz Client and the firmware compile against the same `Quartz-Protocol` definitions for packet layouts, constants and version information. Keeping the protocol shared avoids maintaining separate host/device structures that can silently drift apart.

The device exposes QRPC endpoints for host-side operations such as framebuffer uploads and telemetry. The client keeps the expensive visualization, analysis and reverse-engineering work on the PC while the keyboard firmware remains small and deterministic.

## Building

The project uses CMake and recursively builds the C++ sources under `src/`. Required third-party projects are tracked as submodules.

```bash
git clone --recursive https://github.com/raonygamer/Quartz-Client.git
cd Quartz-Client
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
```

Linux is the primary development platform. Some functionality is inherently Linux-specific, particularly `/proc` process discovery, `process_vm_readv`/`process_vm_writev`, ptrace hardware breakpoints, evdev and the KDE tray integration.

## Disclaimer

Quartz Client is made by **Raony Reis** and is not affiliated with Redragon. The reverse-engineering tools are included because they are useful while developing and experimenting with my own software/hardware environment; they are not intended as a supported general-purpose debugger product.
