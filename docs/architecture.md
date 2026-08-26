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
