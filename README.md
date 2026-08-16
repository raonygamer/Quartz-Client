# Quartz Client

> Native visualization, scripting, diagnostics and reverse-engineering tooling for Quartz-powered devices.

Quartz Client is the desktop companion for my Quartz firmware projects. It started as a small RGB visualizer and gradually became the place where I experiment with device telemetry, shaders, scripting and native-process tooling.

This is a **personal project built for my own hardware and workflow**. It is public because the code and experiments may be useful or interesting to other people; it is not intended to be a polished general-purpose keyboard configurator or supported debugger product.

## Current status

Quartz Client is written in modern **C++20** and primarily developed on **Linux**. The UI is built with **Dear ImGui** and OpenGL, device communication uses raw **libusb**, native disassembly uses **Zydis**, signature scanning uses **libhat**, and scripting is powered by embedded **QuickJS**.

Several reverse-engineering features intentionally use Linux facilities such as `/proc`, `process_vm_readv`, `process_vm_writev`, `ptrace`, hardware debug registers and evdev.

## Device, RGB and diagnostics

- Raw libusb discovery and QRPC communication with Quartz devices
- Shared packet definitions through `Quartz-Protocol`
- Direct RGB framebuffer streaming
- Shader-driven keyboard rendering with live editing and external-file reload
- Supersampled render targets with configurable downsampling
- Runtime shader uniforms sourced from keyboard and host state
- Audio spectrum visualization and audio-reactive effects
- MPRIS player/artwork integration
- Device, USB and QRPC diagnostics
- Firmware performance telemetry and matrix timing probes
- Live keyboard preview
- English and Brazilian Portuguese UI
- Theme-aware presentation, configurable rounding and background opacity

## Scripts workspace

Quartz has an integrated TypeScript/JavaScript workspace for experiments and automation while QuickJS remains the execution runtime.

- Integrated TypeScript/JavaScript editor
- Inline and external scripts
- External-source hot reload
- Generated declarations for the Quartz script API
- `Ctrl+Space` completion
- API/signature hover information
- Syntax and diagnostic markers
- Copyable console, state and storage views
- Semantic hover support in read-only source views
- Persistent script state/storage where exposed by the runtime
- Runtime execution limits so a bad script cannot permanently hang the client

The editor assistance and the execution engine are deliberately separate: authoring can be richer without requiring a heavyweight JavaScript engine in the runtime path.

## Reverse-engineering workspace

The RE tools are designed to hand discoveries to one another rather than act as isolated pages.

### Linked process selection

The scanner, monitor, disassembler, signatures and object experiments can share a selected process. Each process selector can be unlinked when a tool needs to inspect a different target.

Process selectors support search, and the common picker can be focused with `Ctrl+F`.

### Address expressions

Address inputs understand module names and arithmetic, for example:

```text
Astroflux.exe+0x12A934
Terraria.exe+0x3F0*4
(module.dll+0x1000)+0x28
```

`Ctrl+Shift+E` evaluates the expression and replaces it with the resolved hexadecimal address.

Mapped addresses are normally presented module-relative, while raw addresses are zero-padded to the target process address width.

### Memory scanner and watch list

- Cheat-Engine-style initial and next scans
- Integer, floating-point, pointer, boolean, UTF-8/UTF-16 and byte-array values
- Decimal and hexadecimal integer input
- Changed/unchanged/increased/decreased/range comparisons
- Asynchronous scanning on Quartz's shared worker pool
- Live refresh of visible results
- Editable/freezeable watch list
- Whole-row context actions for inspection, disassembly, signatures and hardware monitoring
- Architecture-aware pointer width for x86 and x64 targets
- libhat-backed signature scanning with configurable aligned read chunks

### Hardware access monitor

Quartz can use x86 hardware debug registers to discover code that reads or writes an address.

- Write and read/write data breakpoints
- Grouped access sites and hit counters
- Register snapshots for access hits
- Thread-aware breakpoint setup
- One-shot execution probes for capturing registers at a specific instruction
- Direct handoff from hits to the disassembler and signature tools

### Disassembler

The disassembler is architecture-aware and built around decoded instructions rather than parsing display text.

- x86/x64 Intel-syntax disassembly
- Fixed/resizable address gutter and horizontally scrollable instruction area
- Module-relative address presentation
- Asynchronous lazy memory-window fetching around the viewport
- Prefetched backing windows for smooth long-distance scrolling
- Configurable slow automatic refresh for disassembly and raw bytes
- Asynchronous function-start heuristics with live updates
- Compact function boundary markers
- Back/forward navigation with `Ctrl+Z` / `Ctrl+Y`
- `G` for Go To
- `Ctrl+Click` decoded branch/call targets
- Clickable control-flow arrows
- Separate incoming/outgoing route anchors and lane routing
- Aggregated far/cross-boundary control flow instead of filling the viewport with long rails
- Resizable far-flow inspector with source/destination previews and navigation
- Persistent bookmarks and patch history
- Semantic register and instruction hovers with EN/PT-BR descriptions
- Captured register/flags context when an execution snapshot is available
- Raw-byte view synchronized with the same address-column model

Navigation from an instruction or flow edge recenters the destination and highlights the selected line so the landing point is obvious.

### Assembler and patching

The disassembler also contains a conservative in-place patch workflow.

- Assemble selected instructions/spans
- NOP instructions or selections
- Encoding and replacement-byte preview
- Optional NOP fill for shorter replacements
- Optional extension across following complete instructions
- Pre-write target-byte verification
- Write confirmation controls
- Automatic re-disassembly after patching
- Session patch history with restoration of original bytes
- Separate assembler tweak settings

Quartz does not silently invent code caves or trampolines for oversized replacements.

### Object experiments

Object experiments are defined by TypeScript schemas and then bound to a process/base address. The script definition remains the source of truth instead of maintaining a second native descriptor editor.

```ts
const Vec2 = Struct.define({
    x: Field.Float32(0x0),
    y: Field.Float32(0x4),
});

return Struct.define({
    health: Field.Int32(0x0),
    maxHealth: Field.Int32(0x4),
    position: Field.Struct(0x8, Vec2),
    owner: Field.Pointer(0x10),
    name: Field.CString(0x18, 128),
    wideName: Field.WString(0x98, 128),
});
```

The current `Field` API includes signed/unsigned integer widths, floats, booleans, pointers, nested structs, arrays, `CString` and `WString`. The editor provides completion and hover assistance for the schema DSL.

### Signatures

- Fast asynchronous hexadecimal signature search with libhat
- Wildcards and nibble wildcards
- Signature generation from decoded instructions
- Relocation-sensitive operand wildcarding
- Architecture-aware x86/x64 instruction decoding while deriving patterns
- Pattern extension until a useful unique result is reached
- Results can be handed directly to the rest of the RE workspace

## Typical RE flow

```text
value scan
  -> watch list
  -> hardware access monitor
  -> register capture
  -> disassembly / function exploration
  -> signature or module-relative address
  -> object experiment / script
```

The goal is to keep discovery interactive: a result found in one tool should be useful immediately in the next tool without manually copying half the process state around.

## Configuration

Presentation settings and host-side tooling settings are persisted separately from firmware/device configuration. Current configurable areas include UI appearance, background opacity, signature-scan chunk size, disassembly/raw-byte refresh rate, function-analysis behavior and assembler patch policy.

## Architecture

```text
Quartz Client
├── application / window
├── ui
│   ├── pages
│   ├── editors
│   └── reverse-engineering workspace
├── usb / QRPC
├── native
│   ├── process memory
│   ├── scanner / watchpoints
│   ├── execution probes
│   ├── signatures
│   ├── function analysis
│   └── disassembly / opcode tools
├── runtime / QuickJS
├── shader / rendering
├── audio / media
└── Quartz-Protocol
```

## Building

The project uses CMake and tracks its third-party dependencies as submodules.

```bash
git clone --recursive https://github.com/raonygamer/Quartz-Client.git
cd Quartz-Client
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
```

For a debug build:

```bash
cmake -S . -B build/debug -DCMAKE_BUILD_TYPE=Debug
cmake --build build/debug -j
```

GitHub Actions builds `master` and pull requests as an additional compile check.

## Safety / disclaimer

Quartz Client is made by **Raony Reis** and is not affiliated with Redragon.

The native-process features exist for development and experimentation in my own environment. Writing arbitrary process memory, freezing the wrong value, placing hardware breakpoints on hot code or patching executable bytes can crash or corrupt the target process. Quartz adds previews and verification around several operations, but it cannot make an inherently unsafe experiment safe.
