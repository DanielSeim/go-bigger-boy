# Multi-core architecture

Go Bigger Boy separates emulator cores from applications so desktop, Android,
web, and command-line frontends do not need to know which CPU they are running.
The current GB/GBC implementation is the first adapter; Game Boy Advance can be
added alongside it without replacing the existing core.

```text
Desktop / Android / Web / CLI
             |
       gbb::EmulatorCore
             |
       gbb::CoreRegistry
          /       \
   GB/GBC adapter  future GBA adapter
          |              |
 gameboy_core       gba_core target
```

## Stable frontend boundary

`include/gbb/core.hpp` contains only system-neutral types. `CoreDescriptor`
describes the selected system, video and audio formats, timing, inputs, and
optional capabilities. `EmulatorCore` supplies execution, input, frames, audio,
save states, and persistent cartridge data. Frontends must use these values
instead of assuming 160x144 video, eight buttons, stereo audio, or 70,224 cycles
per frame.

`include/gbb/core_registry.hpp` chooses a core through confidence-based ROM
probes. Every built-in core contributes a `CoreFactory`; the highest-confidence
factory creates the adapter. Stable string system IDs (`gb`, `gbc`, `gba`) are
used in shared metadata instead of C++ enum ordinals.

## Optional scene data

`include/gbb/scene.hpp` defines a read-only `SceneSnapshot` for presentation
renderers that need more than the final framebuffer, such as a voxel diorama.
It contains legacy compatibility fields for the current Game Boy renderer,
plus optional opaque core-defined `SceneLayer` payloads identified by a
namespaced format string. `CoreDescriptor` carries the implementation-owned
list of formats a core may emit; contract validation rejects missing, duplicate,
or unadvertised formats before a frontend consumes the snapshot.
`EmulatorCore::video_frame()` remains the universal fallback, while cores that
can provide scene data advertise `CoreCapability::scene_layers`. The Game Boy
adapter currently exposes both
32x32 background/window maps, both CGB VRAM banks, CGB palette RAM, and decoded
OAM coordinates. No renderer-specific or Game Boy-specific types cross the
frontend boundary.

Scene snapshots are refreshed on request and are read-only; they do not alter
emulation state or save-state data. The SDL voxel renderer consumes this API
to build a perspective mesh and submits it through `SDL_RenderGeometry`, which
uses the active D3D/OpenGL/Metal/Vulkan backend where available. Per-ROM depth
profiles are loaded by the frontend from `voxel-profiles.ini`, leaving the core
independent of presentation tuning. The native Windows dashboard exposes the
active ROM fingerprint and profile fields, and writes only that ROM's section
while preserving profiles for other titles.

`include/gbb/scene_json.hpp` provides the versioned `gbb.scene.v1` JSON
serializer and file exporter. It deliberately contains only JSON primitives
and arrays, including opaque optional layer payloads, so external renderers,
debugging tools, and archival utilities can consume snapshots without linking
to emulator internals. The CLI exposes this as `gbb_cli <rom>
[instruction-count] --scene-json <output>`, and the browser offers the same
export from its ROM screen.

The SDL frontends include an experimental `Voxel diorama` presentation mode.
It uses the snapshot to generate deterministic
perspective tile-column and sprite meshes beneath the authoritative
framebuffer. SDL geometry is intentionally used instead of shipping separate
shader binaries, keeping the renderer portable across desktop backends.

## System-specific tools

Capabilities such as debugger, cheats, printer, camera, and sprite editor are
optional. The normal runtime stays on `EmulatorCore`. A tool may use the adapter
in `include/gbb/gameboy_core.hpp` only after checking the corresponding
`CoreCapability`. This keeps existing GB development tools while allowing a GBA
core to provide a different debugger later.

The optional `link_cable` capability identifies cores that expose a clocked
serial endpoint. Link transports should coordinate two cores outside the
generic frame/audio loop; the Game Boy adapter currently provides the
deterministic in-process `gameboy::SerialCable` for local testing.
The reusable `gameboy::LinkSession` owns that cable and coordinates the two
endpoints' cycle-balanced scheduler; it consumes the core-neutral
`gameboy::LinkEndpoint` (serial port plus one emulation step), while
`GameBoyLinkEndpoint` adapts the built-in Game Boy emulator. Frontends can
observe its lifecycle without depending on serial implementation details.
Network transports can
implement `gameboy::LinkTransport` and use the versioned
`gameboy::LinkPacketCodec` framing without changing the core loop. Socket I/O
must remain asynchronous so a network stall cannot pause CPU emulation. The
non-blocking `gameboy::TcpLinkChannel` supplies loopback host/connect and
framing. `gameboy::TcpSerialEndpoint` binds those queued packets to serial
edges without blocking; a frontend can compose it with one emulator for a
remote session.

## Adding a GBA core

1. Add an independent `gba_core` library with no SDL, Android, or browser code.
2. Implement an `EmulatorCore` adapter and a conservative GBA ROM probe.
3. Register its `CoreFactory` in the application registry and link the target.
4. Describe 240x160 video, GBA timing/audio, and the additional X/Y/L/R inputs
   in its `CoreDescriptor`.
5. Add adapter contract tests, persistence/state tests, and conformance ROMs.
6. Enable only the tools represented by the adapter's capability mask.

The GB implementation remains independently testable as `gameboy_core`; the
frontend-facing target is `gbb_core_api`.

## Dynamic plug-ins

The static C++ API remains the default in-process extension seam. A native
loader and `EmulatorCore` adapter use a separate frozen, fixed-width C ABI with
explicit ownership, result codes, version negotiation, and size-prefixed
tables; exporting `EmulatorCore` directly would make STL and compiler-runtime
details part of the binary contract. The approved v1.0 contract, fixture, and
loader limits are documented in [`plugin-abi.md`](plugin-abi.md) and its
[`freeze record`](plugin-abi-freeze.md). Automatic production discovery and
frontend exposure remain deferred pending a separate security and UX review.

Save-state framing and checksum validation are isolated from hardware field
serialization. CPU, cartridge, joypad, timer, PPU, and APU fields are delegated
through private codec boundaries, so changes to those state groups do not
require editing the public emulator entry points. The complete bus payload,
including version-gated migrations, is delegated to `SaveStateBusCodec`.

## Migration status

The browser runtime, CLI, and SDL's ordinary single-player path now create and
run cores through this boundary. SDL owns `EmulatorCore`; its concrete Game Boy
pointer is non-owning and is used only by mature GB-only debugger, movie, TAS,
sprite, camera, link, and cheat tools. Those tools remain intentionally outside
the neutral API and are enabled only when the corresponding adapter exists.
The SDL shell still has a few Game Boy-specific UI assumptions in linked and
development-tool paths, so exposing a second core there remains a follow-up
task rather than a claim that every optional tool is portable.
