# Quartz Client

> A native configuration, visualization, diagnostics, scripting and reverse-engineering companion for Quartz-powered devices.

Quartz Client is the desktop companion for my Quartz firmware projects. It talks directly to Quartz devices over the custom USB RPC interface, streams RGB framebuffers, exposes firmware diagnostics, drives shader-based effects and contains the host-side tools I use while developing the firmware and experimenting with native applications.

It started as a small RGB visualizer/client and **ended up being a minified Cheat Engine by fun**.

This is a **personal project written for my own devices and my own usage**. It is public because someone else might find it useful or interesting, but I do not intend to turn it into a general-purpose keyboard configuration utility or a supported debugger product.

## Current status

Quartz Client is actively developed and used alongside the Quartz K552X firmware. The original monolithic prototype has been split into dedicated UI, USB/RPC, rendering, scripting, runtime-binding, process-tooling, profile and platform subsystems.

The client is written in modern **C++20** and currently uses **OpenGL**, **Dear ImGui**, raw **libusb**, **Zydis**, **libhat** and embedded **QuickJS**.

Linux is the primary development platform. Several reverse-engineering and integration features intentionally depend on Linux facilities such as `/proc`, `process_vm_readv`, `process_vm_writev`, `ptrace`, hardware debug registers and evdev.

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

### RGB, audio and shaders

- OpenGL shader-driven keyboard framebuffer rendering
- Live fragment/vertex shader editing
- Shader catalog plus external-file hot reload
- Runtime uniforms sourced from keyboard state, bindings and external data
- Supersampled keyboard framebuffer with configurable downsampling
- Audio-reactive effects and spectrum visualization
- MPRIS media artwork colors and player selection
- Global brightness and base-color controls
- Live keyboard preview
- Theme-aware editor surfaces and configurable UI rounding/background presentation

### Runtime bindings and automation

Quartz exposes host/device/application state through a runtime binding graph rather than hard-coding every experiment into the renderer.

- Native-process values and addresses
- Signature-resolved addresses
- Binding-to-binding sources
- Object fields and object status
- Aggregates and comparisons
- Value bank
- Shader/render state
- Device/RPC/USB telemetry
- Keyboard and audio state
- Controls with conditions, actions and nested grouping
- Profiles and global profile hotkeys through the Quartz evdev stream (GLFW fallback)
- Runtime status/error feedback and rescan/rebind operations
- Embedded QuickJS scripted bindings with persistent per-binding state and execution deadlines
- Script-side `q.re` process/memory/signature/disassembly helpers

#### QuickJS scripted bindings

A binding can use a QuickJS script when a graph expression is easier to describe in code. The script receives the Quartz runtime API and is compiled once, then reused at the binding's configured update rate.

```js
const shield = q.binding("Player Shield") ?? 0;
const maxShield = q.binding("Player Max Shield") ?? 1;

q.state.low ??= false;
q.state.low = shield / maxShield < 0.25;

return {
    value: shield / maxShield,
    string: q.state.low ? "LOW" : "OK"
};
```

`q.state` persists between updates, exact addresses are represented as JavaScript `BigInt`, and scripts can return scalar values or richer `{ value, string, address }` results. The embedded runtime intentionally does not expose general filesystem/network helpers and uses execution limits so a bad loop cannot permanently hang the client.

### Scripts workspace

Quartz also has a first-class script workspace for larger experiments and automation.

- Integrated TypeScript/JavaScript editor
- External script storage/import workflow
- Generated declarations for the exposed Quartz APIs
- Completion popup with `Ctrl+Space`
- API hover/signature help
- Syntax/diagnostic markers in the editor
- Script console/state/storage views with copy support
- Read-only source views with the same semantic hover support where applicable

The script editor and runtime are separate concerns: editor assistance exists to make authoring pleasant, while QuickJS remains the execution path.

## Reverse-engineering workspace

The reverse-engineering side is intentionally built as one connected workspace rather than a pile of unrelated utilities. Most process selectors can be **linked**, so choosing a process in the disassembler can automatically select it in the scanner, monitor, signature tools and object experiments. Any selector can be switched back to local mode when an independent target is useful.

### Addresses

Address inputs understand more than raw hexadecimal values. Expressions support module names and basic arithmetic:

```text
Astroflux.exe+0x12A934
Terraria.exe+0x3F0*4
(module.dll+0x1000)+0x28
```

`Ctrl+Shift+E` evaluates an address expression and replaces it with the resulting hexadecimal address.

When an address belongs to a mapped module, Quartz prefers **module-relative presentation** such as:

```text
Astroflux.exe  +0x12A934
```

instead of filling the screen with unrelated absolute addresses. Absolute values remain available through hovers/context actions.

### Memory scanner and watches

- Cheat-Engine-style value scanning with next-scan filtering
- Decimal and `0x` hexadecimal integer input
- Live refresh of visible results
- Previous-scan comparisons with change coloring
- Editable watch list with configurable types, addresses and values
- Value freezing at a configurable refresh rate
- Manual watch creation without running a fake scan first
- Whole-row context actions for inspection, disassembly, signatures and hardware monitoring
- Asynchronous scanning work on Quartz's shared worker pool
- libhat-backed signature scanning with configurable, aligned memory-copy chunk size

### Hardware access monitoring

Quartz can answer the classic "what code is touching this address?" question with x86 hardware debug registers.

- Write and read/write hardware data breakpoints
- Grouped access sites with hit counters
- Latest register snapshot per access site
- Newly created threads mirrored into the active watch
- One-shot execution probes for capturing registers at a specific instruction
- Direct handoff from an access hit to disassembly/signature generation

Hardware data breakpoints trap after the memory access on x86, so the captured instruction pointer is treated accordingly in the UI.

### Disassembler

The disassembler is architecture-aware and backed by Zydis.

- x86/x64 Intel-syntax disassembly
- Module-relative address columns with stable alignment
- Function hints discovered asynchronously on the shared worker pool
- Function headers such as `fn_12A900` without hiding the real module-relative address
- Background heuristics that update the visible view without blocking the UI thread
- Lazy/infinite scrolling using a prefetched backing window around the viewport
- Contiguous readable mappings treated as one scrollable neighborhood
- Configurable slow automatic refresh for disassembly and raw bytes
- Persistent bookmarks/markers with tags and custom colors
- Bookmark tab for fast `Go to`
- `G` for the Go To popup
- Back/forward navigation (`Ctrl+Z` / `Ctrl+Y`)
- `Ctrl+Click` a decoded branch/call to follow its target
- Clickable control-flow arrows with navigation history
- Branch routes that avoid instruction text and spread conflicting paths across separate rails
- Semantic instruction/register hover information
- Captured register values shown when a one-shot snapshot exists, otherwise explicitly marked uncaptured
- Branch-condition explanations when the exact instruction has a matching captured flags snapshot
- Raw-byte view synchronized with the disassembler

Control-flow visualization is deliberately based on decoded targets rather than parsing the prettified assembly text, so function labels and module-relative formatting do not break navigation.

### Assembler and patching

The disassembler includes an in-place assembler/patch workflow.

- Assemble a selected instruction/span
- NOP an instruction or selection
- Encoding preview before writing
- Original/replacement byte preview
- Optional NOP fill for shorter replacements
- Optional extension over following whole instructions
- Pre-write verification that target bytes have not changed
- Configurable write confirmation
- Automatic re-disassembly after a patch
- Session patch history with restoration of saved original bytes
- Separate **Assembler Tweaks** settings

This is intentionally conservative. Oversized replacements are rejected unless the configured policy can safely extend across complete following instructions. Quartz does not silently invent trampolines/code caves behind your back.

### Object experiments

Object experiments are **script-defined**, not persistent C++ descriptors. A small TypeScript program returns a `Struct.define({...})` schema and Quartz binds that definition to a selected process/base address.

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

The embedded `Field` API currently includes signed/unsigned integer widths, floats, booleans, pointers, nested structs, arrays, `CString` and `WString`. The editor provides completion/hover assistance for the DSL and the evaluated definition remains the source of truth.

Live object rows can be inspected or handed back into the rest of the RE workflow without maintaining a separate native descriptor editor.

### Signatures

- Fast asynchronous hexadecimal signature search using libhat
- Wildcards and nibble wildcards
- Signature maker from decoded instructions
- Automatic wildcarding of relocation-sensitive operands
- Pattern extension until a useful unique result is reached
- Results can feed the inspector, watch list, hardware watcher or runtime bindings

## A typical RE workflow

The tools are meant to feed one another:

```text
value scanner
    -> watch list
    -> hardware access watch
    -> captured registers
    -> disassembler / function exploration
    -> signature or module-relative address
    -> object experiment / runtime binding
    -> shader or control
```

For example, if a game displays `61` shield:

1. Scan for `61`, damage the player, then Next Scan with the new value.
2. Add the surviving result to the watch list.
3. Watch writes/accesses to discover the instruction changing it.
4. Capture registers when that instruction executes.
5. Use the captured object pointer to test neighboring fields in Object Experiments.
6. Build a signature or other runtime resolver so the discovery survives process restarts/ASLR.
7. Expose the result as a named binding such as `Player Shield`.
8. Let a shader/control consume the binding without caring how the value was discovered.

That separation is the main design goal: the messy process-memory work happens at the discovery/resolution layer, while the rest of Quartz sees clean named runtime values.

## UI and configuration

Quartz persists presentation and runtime-tool preferences separately from the firmware/device configuration.

- English and Portuguese (Brazil) UI
- Theme-aware controls/editors
- Configurable corner rounding
- Background presentation controls
- Signature-scan chunk tuning
- Disassembler/raw-byte refresh rates
- Background function-analysis controls
- Assembler patch behavior

The UI is still actively evolving, so labels/tooltips and translated strings receive frequent cleanup passes as new tools land.

## Architecture

```text
Quartz Client
├── application / window
├── ui
│   ├── pages
│   ├── editors
│   ├── memory inspector
│   └── reverse-engineering workspace
├── device state
├── usb
│   ├── raw libusb transport
│   └── QRPC session
├── native
│   ├── process/memory access
│   ├── memory scanner
│   ├── hardware watchpoints
│   ├── execution probes
│   ├── signature scanner
│   ├── function analysis
│   └── disassembly / opcode tools
├── runtime
│   ├── bindings
│   ├── scripting / QuickJS
│   ├── controls
│   ├── profiles
│   ├── value bank
│   └── object experiments
├── shader / rendering
├── audio / media
└── Quartz-Protocol
```

## Protocol

Quartz Client and the firmware compile against the same `Quartz-Protocol` definitions for packet layouts, constants and version information. Keeping the protocol shared avoids maintaining separate host/device structures that can silently drift apart.

The device exposes QRPC endpoints for host-side operations such as framebuffer uploads and telemetry. The client keeps expensive visualization, analysis, scripting and reverse-engineering work on the PC while the keyboard firmware remains small and deterministic.

## Building

The project uses CMake and tracks required third-party projects as submodules.

```bash
git clone --recursive https://github.com/raonygamer/Quartz-Client.git
cd Quartz-Client
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
```

For development builds:

```bash
cmake -S . -B build/debug -DCMAKE_BUILD_TYPE=Debug
cmake --build build/debug -j
```

GitHub Actions also builds the active development branch used for the QuickJS/RE work.

## Safety / disclaimer

Quartz Client is made by **Raony Reis** and is not affiliated with Redragon.

The reverse-engineering features are included because they are useful while developing and experimenting with my own software/hardware environment. Writing arbitrary process memory, freezing the wrong value, placing hardware breakpoints on hot code or patching executable bytes can crash or corrupt the target process. Quartz previews and verifies several operations, but it cannot make an inherently unsafe experiment safe.
