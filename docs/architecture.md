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
It contains generic tile layers, tile graphics, palettes, sprites, LCD state,
and emulation timing. `EmulatorCore::video_frame()` remains the universal
fallback, while cores that can provide scene data advertise
`CoreCapability::scene_layers`. The Game Boy adapter currently exposes both
32x32 background/window maps, both CGB VRAM banks, CGB palette RAM, and decoded
OAM coordinates. No renderer-specific or Game Boy-specific types cross the
frontend boundary.

Scene snapshots are refreshed on request and are read-only; they do not alter
emulation state or save-state data. The SDL voxel renderer consumes this API
to build a perspective mesh and submits it through `SDL_RenderGeometry`, which
uses the active D3D/OpenGL/Metal/Vulkan backend where available. Per-ROM depth
profiles are loaded by the frontend from `voxel-profiles.ini`, leaving the core
independent of presentation tuning.

The SDL frontend now includes an experimental `Voxel diorama (desktop
prototype)` presentation mode. It uses the snapshot to generate deterministic
perspective tile-column and sprite meshes beneath the authoritative
framebuffer. SDL geometry is intentionally used instead of shipping separate
shader binaries, keeping the renderer portable across desktop backends.

## System-specific tools

Capabilities such as debugger, cheats, printer, camera, and sprite editor are
optional. The normal runtime stays on `EmulatorCore`. A tool may use the adapter
in `include/gbb/gameboy_core.hpp` only after checking the corresponding
`CoreCapability`. This keeps existing GB development tools while allowing a GBA
core to provide a different debugger later.

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

## Migration status

The browser runtime and CLI create cores through this boundary. The desktop and
Android SDL shell still contains mature GB-only debugger, movie, TAS, sprite,
printer, camera, and cheat code. Those tools are intentionally not part of the
neutral API; before exposing a second core in that shell, its normal execution,
input, media, state, and persistence paths must move to `EmulatorCore`, with
each existing tool enabled only when its capability and GB adapter are present.
Keeping this limitation explicit prevents a future GBA implementation from
copying GB constants into another frontend.
