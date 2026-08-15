# Quartz Client

> A native configuration and control application for Quartz-powered devices.

Quartz Client is the desktop companion for my Quartz firmware projects.

It communicates with Quartz devices over the custom USB RPC interface and provides configuration, diagnostics, RGB control, profiling, calibration, and other host-side tools.

The goal is to keep the client lightweight, fast, and close to the hardware while sharing protocol definitions directly with the firmware through `Quartz-Protocol`.

The project is written in modern **C++** and will use **OpenGL** and **Dear ImGui** for the user interface.

This is a **personal project written for my own devices and my own usage**. It's public because someone else might find it useful or interesting, but I do **not** intend to turn it into a general-purpose keyboard configuration utility or support arbitrary devices.

If support for another device ever appears, it'll be because I personally wanted or needed it.

## Goals

- Native C++ application
- Lightweight and responsive UI
- Direct USB communication with Quartz firmware
- Shared protocol definitions through `Quartz-Protocol`
- Real-time RGB control
- Audio visualization
- Media-based lighting
- Device configuration
- Performance diagnostics
- Firmware profiling
- Per-key RGB calibration
- Minimal dependencies
- Cross-platform where reasonably possible

## Current Status

🚧 Very early development.

Currently working on:

- USB device discovery
- Quartz RPC transport
- Packet handling
- RGB framebuffer streaming
- Performance statistics
- Protocol sharing with `Quartz-Protocol`
- OpenGL setup
- Dear ImGui interface

## Features

### RGB Control

Quartz Client can communicate directly with the firmware RGB engine.

Planned functionality includes:

- Static colors
- Direct framebuffer streaming
- Audio visualization
- Media artwork colors
- Firmware-side effects
- Per-key control
- Per-key color correction and calibration

### Device Configuration

Firmware settings exposed through RPC can be configured without rebuilding or reflashing the firmware.

The exact set of available settings depends on the connected device and firmware version.

### Diagnostics

The client can inspect runtime information exposed by the firmware, such as:

- Core clock
- Matrix scan timing
- Scan frequency
- CPU usage
- HID processing time
- USB/RPC statistics
- Other device-specific diagnostics

### Profiling

Quartz firmware will expose lightweight profiling events through RPC.

The client will eventually visualize these events directly, allowing firmware execution timing to be inspected without dedicated debugging hardware.

The firmware only needs to collect and transmit compact profiling data; the expensive visualization work stays on the host.

### Calibration

Quartz Client will provide interactive calibration tools for hardware that benefits from per-device or per-key adjustment.

For example, RGB calibration can allow a key to be selected physically and corrected live from the client without manually editing firmware tables.

## Protocol

Quartz Client communicates with devices using the shared `Quartz-Protocol` library.

Both the firmware and client compile against the same packet definitions, protocol constants, version information, and shared data structures.

This avoids maintaining separate protocol implementations that can silently drift apart.

Before normal communication begins, the client and device perform a protocol handshake so incompatible versions can fail cleanly instead of attempting to interpret incompatible packet layouts.

## Architecture

The project will roughly follow this structure:

```text
Quartz Client
│
├── UI
│   └── Dear ImGui
│
├── Device / Application Logic
│
├── RPC
│   └── Quartz-Protocol
│
├── USB Transport
│   └── libusb
│
└── Rendering
    └── OpenGL
