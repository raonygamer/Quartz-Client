# Quartz Client

> A native configuration, visualization, diagnostics and reverse-engineering companion for Quartz-powered devices.

Quartz Client is the desktop companion for my Quartz firmware projects. It talks directly to Quartz devices over the custom USB RPC interface, streams RGB framebuffers, exposes firmware diagnostics and provides the host-side tooling I use while developing the firmware and experimenting with native applications.

It started as a small RGB visualizer/client and **ended up being a minified Cheat Engine by fun**.

This is a **personal project written for my own devices and my own usage**. It is public because someone else might find it useful or interesting, but I do not intend to turn it into a general-purpose keyboard configuration utility or support arbitrary hardware. If support for another device ever appears, it will be because I personally wanted or needed it.

## Current status

Quartz Client is actively developed and already used alongside the Quartz K552X firmware. The codebase has moved well past the original monolithic `Main.cpp`: UI, USB/RPC, native-process tooling, runtime bindings, profiles, rendering, device state and platform integration now live in dedicated subsystems.

The application is written in modern **C++20** and uses **OpenGL**, **Dear ImGui**, raw **libusb**, **Zydis**, **libhat** and embedded **QuickJS**.

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
- Profiles and global profile hotkeys through the Quartz evdev stream (GLFW fallback)
- Runtime status/error feedback and rescan/rebind operations
- Embedded QuickJS scripted bindings with persistent per-binding state, execution deadlines and generated TypeScript declarations
- Script-side `q.re` process/memory/signature/disassembly APIs and bounded loop helpers
- QuickJS scripted bindings with persistent per-binding state, binding/control/value-bank lookup helpers and execution timeouts

#### QuickJS scripted bindings

A binding can use **QuickJS script** as its source when a graph expression is easier to describe in code. The script is a function body and receives a small `q` API; it is compiled once and then reused at the binding's normal `UpdateHz`.

```js
const shield = q.binding("Player Shield") ?? 0;
const maxShield = q.binding("Player Max Shield") ?? 1;
q.state.low ??= false;
q.state.low = shield / maxShield < 0.25;
return { value: shield / maxShield, string: q.state.low ? "LOW" : "OK" };
```

Scripts can read binding values/raw values/strings/addresses, value-bank entries and control state. `q.state` persists between updates, exact addresses are exposed as JavaScript `BigInt`, and scripts may return a number, boolean, string, BigInt address or `{ value, string, address }`. There are deliberately no QuickJS filesystem/network helpers wired in. Each binding also has a short execution deadline so a bad loop does not permanently hang the client.

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

## Reverse-engineering tools, explained like I am five

The reverse-engineering side of Quartz is easiest to understand if every tool is treated as answering one very small question.

A **scanner** answers: "where is this value right now?"

A **watch-list entry** answers: "what is stored at this address right now, and can I keep looking at it?"

A **hardware access watch** answers: "what code is reading or writing this address?"

A **one-shot execution probe** answers: "what are the CPU registers the next time this instruction runs?"

A **disassembler** answers: "what instructions are these bytes supposed to be?"

A **signature** answers: "how can I find this useful code/address again after the program moves around?"

An **object model** answers: "if this address is the start of a `Player` or `Ship`, what do the fields around it mean?"

A **binding** answers: "how do I turn some changing value/address into a named Quartz runtime value?"

A **control** answers: "when some condition involving runtime values happens, what should Quartz do?"

The important part is that these are not separate toys. The normal workflow is to discover something with one tool and then hand it to the next one.

Everything below uses a completely fictional process called `ExampleGame`. Assume it has a player ship with health/shield values. The addresses and assembly are intentionally fake.

### Tutorial 1: find a value and keep watching it

Suppose `ExampleGame` shows the ship shield as `73` on screen and you want to find where that number lives.

1. Open **Reverse Engineering → Memory Scanner**.
2. Select `ExampleGame`.
3. Pick the likely type. If the value looks like a normal whole number, `i32` is a decent first guess.
4. Enter `73` and run **New Scan**.
5. Go back to the game and lose a little shield. Suppose it becomes `61`.
6. Enter `61` and run **Next Scan**.
7. Repeat until the result list is small enough to inspect.

Imagine the remaining result is:

```text
0x51A4C2AC = 61
```

Right-click the value and choose **Add to watch list**.

You now have a persistent row that Quartz refreshes for you. The scanner did its job and can be forgotten; the watch list is now the convenient place to stare at the value, change its type/address/value, or temporarily freeze it while testing.

If you already know an address from some other source, you do not need to run a fake scan just to reach the watch list. Use **Manual Add to watch list**, enter the PID/address/type, and Quartz creates the same kind of watched entry directly.

### Tutorial 2: turn that address into a binding

A watch-list entry is useful for a human looking at the screen. A **binding** is useful when Quartz itself needs the value.

Suppose `0x51A4C2AC` really is current shield and you want your keyboard shader to react to it.

Right-click the watched value and choose **Create binding**. Conceptually you have just created something like:

```text
Binding name: Player Shield
Source: Native process memory
Process: ExampleGame
Address: 0x51A4C2AC
Type: i32
```

The mental model is simply:

```text
ExampleGame memory -> Player Shield binding -> anything in Quartz
```

The raw address is not very expressive. `Player Shield` is.

Once the binding is producing a value, you can use it as a source for shader/material state, another binding, a comparator, a control, a value-bank entry, or whatever else understands runtime bindings.

For example, a shader can effectively receive:

```text
Player Shield = 61
```

instead of knowing anything about Linux process memory.

### Tutorial 3: make a control from a binding

Bindings are values. **Controls** are rules.

Suppose the fictional ship has `100` maximum shield and you want Quartz to switch to a warning shader whenever shield drops below `25`.

First make sure you have a `Player Shield` binding. Then create a control with roughly this meaning:

```text
Input: Player Shield
Condition: value < 25
Target: Active shader
Value: Low Shield Warning
```

Read that literally as a sentence:

> When `Player Shield` is less than `25`, make `Low Shield Warning` the active shader.

That is basically what a control is. It takes an input, asks a question about it, then performs an action.

Controls can also trigger binding operations, toggle other runtime state, write value-bank entries, react only on edges/changes, or execute extra actions. The simple version is still the useful mental model:

```text
WHEN <condition involving runtime data>
DO   <something in Quartz>
```

A **No operation** target is useful when the control exists mainly to run its extra actions and should not have a meaningful primary target.

### Tutorial 4: discover what code changes a value

Knowing that shield lives at `0x51A4C2AC` is useful, but the address itself does not explain how the game works.

Right-click the watched shield value and choose **Watch accesses**. Select a write or read/write hardware watchpoint depending on what you want to discover.

Quartz uses the CPU debug registers instead of continuously polling for code changes. When some instruction touches the address, the watch page records the access site.

You might get grouped results such as:

```asm
0x00491A20  mov [ecx+0xAC], eax    Hits: 3
0x00492F10  mov eax, [ecx+0xAC]    Hits: 47
0x00494061  cmp [edx+0xAC], 0      Hits: 11
```

Quartz groups repeated traps by the instruction that caused them, so the list does not grow forever when one hot instruction executes thousands of times. `Hits` tells you how often that access site was observed.

This is already a useful clue: several completely different instructions using `+0xAC` suggests that `0xAC` may be a real field offset inside some object rather than a random absolute address.

### Tutorial 5: use a one-shot execution probe to find the object pointer

Suppose this access looks interesting:

```asm
mov eax, [ecx+0xAC]
```

The obvious next question is: **what is ECX?**

Right-click the instruction and choose **Capture registers when executed**.

Quartz arms a one-shot hardware execution breakpoint. The line is marked red with `armed for next execution`. The process keeps running normally. The next time that instruction executes, Quartz captures the register state, automatically disarms the breakpoint and resumes the process.

Imagine the register tab now shows:

```text
EAX = 0x0000003D
ECX = 0x51A4C200
EDX = 0x00000000
EIP = 0x00492F10
...
```

The instruction was:

```asm
mov eax, [ecx+0xAC]
```

and Quartz captured:

```text
ECX = 0x51A4C200
```

So the CPU is effectively doing:

```text
read 4 bytes from 0x51A4C200 + 0xAC
```

which is:

```text
read 4 bytes from 0x51A4C2AC
```

That is the exact shield address you found earlier.

This is how a random scanner result can turn into a much more useful hypothesis:

```cpp
struct Ship
{
    // ... unknown stuff ...
    std::int32_t Shield; // +0xAC
};
```

Right-clicking register values lets you immediately inspect/disassemble/use them as addresses instead of manually copying them between tools.

### Tutorial 6: mark useful assembly instead of rediscovering it every 30 seconds

While browsing disassembly, right-click an interesting instruction and add a marker.

For example:

```text
0x00492F10  [cyan]  ship shield read
0x00491A20  [red]   ship shield write
0x00495000  [green] confirmed ship update
```

Cyan is the default marker color, but each marker can have its own color and tag. The entire line gets tinted, which makes it much easier to visually navigate a function once you have identified a few important instructions.

An armed one-shot execution probe temporarily takes precedence and marks the target line red with `armed for next execution`. After the probe fires or is cancelled, your normal marker comes back.

The lower inspector area has **Raw bytes**, **Registers** and **Patch bytes** tabs. Raw bytes and disassembly can synchronize their selected address, so clicking around one view can keep the other view pointed at the same location.

`Patch bytes` is intentionally very literal: it writes bytes into the target process. It is useful for experiments, but there is no magical undo if the target does not like your experiment.

### Tutorial 7: make the useful instruction survive ASLR with a signature

Hard-coded addresses such as `0x00492F10` are often not stable between launches or versions. What you actually care about may be the **bytes/instruction pattern** around that address.

Suppose the disassembly around your shield reader looks like this:

```asm
8B 81 AC 00 00 00       mov eax, [ecx+0xAC]
85 C0                   test eax, eax
7E 0A                   jle short ...
89 45 F4                mov [ebp-0xC], eax
```

A simple signature could be:

```text
8B 81 AC 00 00 00 85 C0 7E ?? 89 45 F4
```

The `??` means "I do not care about this byte." Wildcards are useful for relative branches, addresses, relocations, or other bytes that may change while the surrounding code remains recognizable.

Open **Reverse Engineering → Quick signature search**, select `ExampleGame`, paste the pattern and scan.

If Quartz finds:

```text
0x0051D8B0
```

that result can immediately be:

- inspected in memory,
- opened in the disassembler,
- added to the value watch list,
- watched for accesses,
- or turned into a runtime signature/address binding.

The last option is the important one for long-lived runtime behavior. Instead of saying:

```text
Player shield accessor lives at 0x00492F10 forever
```

you can say:

```text
Find this byte pattern at runtime, then use the resolved result
```

That is the difference between a quick reverse-engineering observation and something Quartz can attempt to rediscover automatically the next time the process starts.

### Tutorial 8: build an object model from neighboring fields

Suppose continued testing suggests this layout:

```text
Ship object base = 0x51A4C200
+0xA8 = current hull
+0xAC = current shield
+0xB0 = maximum shield
+0xB4 = energy
```

Instead of keeping four unrelated addresses in your head, create an object descriptor such as `Ship`.

Conceptually:

```cpp
struct Ship
{
    // unknown bytes before the interesting area
    std::int32_t Hull;       // +0xA8
    std::int32_t Shield;     // +0xAC
    std::int32_t MaxShield;  // +0xB0
    std::int32_t Energy;     // +0xB4
};
```

The real descriptor UI supports explicit/manual offsets, filler fields, alignment/packing choices, pointers and other field types, so you do not need to know every byte of the object before modeling the interesting parts.

Now open **Reverse Engineering → Object model debugger**:

1. Select `ExampleGame`.
2. Select the `Ship` descriptor.
3. Enter `0x51A4C200` as the base address.
4. Bind it.

Quartz will show the model as live fields instead of unrelated memory:

```text
Field       Offset   Address       Value
Hull        +0xA8    0x51A4C2A8    240
Shield      +0xAC    0x51A4C2AC     61
MaxShield   +0xB0    0x51A4C2B0    100
Energy      +0xB4    0x51A4C2B4     87
```

Right-click a field and it can go straight back into the normal tools: inspect it, disassemble there, add it to the watch list, watch accesses, create a direct binding, or follow a pointer-looking value.

The debugger is deliberately temporary. If the object/base becomes useful enough to keep, **Save as pointer instance** promotes the idea into the persistent runtime object/pointer system.

Quartz also uses the target process pointer width while laying out debug objects. A 32-bit `ExampleGame` gets 4-byte pointer fields even if Quartz itself is a 64-bit process.

### Tutorial 9: bind a field through an object model instead of a raw address

A raw binding says:

```text
read i32 from 0x51A4C2AC
```

An object-field binding can say something conceptually closer to:

```text
take the current Ship instance
read its Shield field
```

This becomes more useful once the object base itself is resolved dynamically.

Imagine a separate binding called `Local Ship Address` finds or captures the ship object base. A persistent `Ship instance` pointer can use that binding as its base. Then a `Ship.Shield` object-field binding refers to the `Shield` field on that pointer instance.

The dependency chain becomes:

```text
signature/register capture
        |
        v
Local Ship Address binding
        |
        v
Ship instance (object pointer)
        |
        v
Ship.Shield object-field binding
        |
        +----> shader
        +----> control
        +----> comparator
        +----> value bank
```

That is much more robust and readable than scattering `base + 0xAC`, `base + 0xB0`, `base + 0xB4` calculations around unrelated bindings.

### Tutorial 10: one complete fictional workflow

Here is the entire idea end-to-end.

You want the keyboard to turn red when a fictional game's player ship is low on shield.

**Discovery:**

```text
Search 100
-> shield takes damage
Search 82
-> shield takes damage
Search 61
-> one useful address remains
```

**Watch:**

```text
0x51A4C2AC = 61
```

Add it to the watch list so you can observe it conveniently.

**Find the code:**

Use a hardware read/write watch and discover:

```asm
mov eax, [ecx+0xAC]
```

**Find the object:**

Arm a one-shot execution probe on that instruction and capture:

```text
ECX = 0x51A4C200
```

Now you know that the watched address is `object + 0xAC`.

**Explore neighbors:**

You notice `object + 0xB0` behaves like maximum shield and `object + 0xB4` behaves like energy.

**Model it:**

Create a `Ship` descriptor and bind it temporarily in the Object model debugger. Confirm that the fields continue behaving like you expect.

**Make rediscovery stable:**

Create a signature or register-capture binding that resolves the local ship object after every process launch instead of relying on yesterday's absolute address.

**Create runtime values:**

Create object-field bindings:

```text
Ship.Shield
Ship.MaxShield
```

Optionally create another binding that normalizes them into a fraction:

```text
Shield fraction = Ship.Shield / Ship.MaxShield
```

**Automate Quartz:**

Create a control:

```text
WHEN Shield fraction < 0.25
DO   switch to Low Shield Warning shader
```

At that point the reverse-engineering tools have disappeared from the final behavior. The shader/control system just sees clean named runtime values. The ugly process-memory discovery work exists only underneath the bindings that supply them.

That is the main design goal of the runtime system: use the RE tools to discover **where data comes from**, then hide the messy address/debugger details behind bindings so the rest of Quartz can treat the result like any other signal.

### A few practical rules

- Start with the value scanner when you know a **value** but not an address.
- Start with Manual Add to watch list when you already know the **address**.
- Use a hardware access watch when you know an address but want to know **which code uses it**.
- Use a one-shot execution probe when you know an instruction but want to know **what its registers point at**.
- Use the disassembler/markers when you are trying to understand **code around a discovery**.
- Use quick signatures when you found useful code and want to see whether a byte pattern can **find it again**.
- Use object models when several values appear to be **fields of the same thing**.
- Use bindings once Quartz itself should consume the discovery.
- Use controls when Quartz should **react** to one or more bindings.
- Prefer signatures/object pointers over permanent hard-coded absolute addresses whenever the target can move between launches.

These tools can obviously crash or corrupt the target if you write nonsense, freeze the wrong field, patch arbitrary code or use incorrect object layouts. Quartz does not pretend that process-memory experimentation is safe just because the UI has buttons for it.

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
