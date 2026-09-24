<p align="center">
  <img src="go_bigger_boy_logo.png" alt="Go Bigger Boy (GBB)" width="720">
</p>

<p align="center">
  <a href="https://github.com/DanielSeim/go-bigger-boy/actions/workflows/desktop-builds.yml"><img src="https://github.com/DanielSeim/go-bigger-boy/actions/workflows/desktop-builds.yml/badge.svg" alt="Desktop builds"></a>
  <a href="https://github.com/DanielSeim/go-bigger-boy/actions/workflows/hardware-model-matrix.yml"><img src="https://github.com/DanielSeim/go-bigger-boy/actions/workflows/hardware-model-matrix.yml/badge.svg" alt="Hardware model matrix"></a>
  <a href="https://github.com/DanielSeim/go-bigger-boy/actions/workflows/android-build.yml"><img src="https://github.com/DanielSeim/go-bigger-boy/actions/workflows/android-build.yml/badge.svg" alt="Android build"></a>
  <a href="https://github.com/DanielSeim/go-bigger-boy/actions/workflows/web-pages.yml"><img src="https://github.com/DanielSeim/go-bigger-boy/actions/workflows/web-pages.yml/badge.svg" alt="Web build and Pages"></a>
</p>

<p align="center">
  <a href="https://danielseim.github.io/go-bigger-boy/"><strong>Try the latest web build on GitHub Pages</strong></a>
</p>

A portable C++17 Game Boy and Game Boy Color emulator core with desktop,
Android, and Web frontends. The core is shared across those frontends; native
Switch support is not currently part of the project.

## Current status

Go Bigger Boy is usable for development, testing, and many real Game Boy and
Game Boy Color sessions, but it is not yet a claim of complete commercial-game
compatibility. The checked-in release gate currently passes **168/168 cases**
covering conformance ROMs, framebuffer comparisons, and core contracts. A full
local CTest run currently reports **226/226 tests passing**, with three
network-dependent tests intentionally skipped when no peer is available. See
the [accuracy report](docs/accuracy.md) for the suite-by-suite breakdown.

What works well:

- Cartridge loading and basic header parsing
- Initial DMG memory map, including work RAM echo behavior
- Complete legal CPU opcode set, including all 256 CB-prefixed operations
- Interrupt dispatch, EI delay, HALT/STOP states, HALT bug, and machine-cycle bus timing
- Cycle-driven DIV/TIMA/TMA/TAC timer with overflow interrupts and write-edge behavior
- DMG PPU modes, LCD/STAT interrupts, VRAM/OAM arbitration, and RGBA framebuffer
- Background, window, and 8×8/8×16 sprite scanline rendering, including the
  window's internal line counter and variable Mode 3 fetch timing
- Game Boy Color mode with banked VRAM/WRAM, RGB555 palettes, tile attributes,
  CGB sprite priority, VRAM DMA, fast serial, and double-speed CPU switching
- Super Game Boy software detection with a clean-room SGB adapter, bit-level
  JOYP command framing, color palettes, tile attribute mapping, screen-data
  CHR_TRN/PCT_TRN/PAL_TRN/ATTR_TRN transfer latches, a
  deterministic 256×224 SNES border framebuffer, and MASK_EN viewport masking
  (SNES audio and boot animation remain future work)
- Active-low joypad matrix with keyboard/gamepad input and interrupts
- Cycle-timed OAM DMA with source-bus conflicts and an optional SDL3 desktop frontend
- Four-channel DMG/CGB audio with a cycle-integrated high-pass mixer and 48 kHz
  stereo SDL3 playback
- ROM-only, MBC1/MBC1M, MBC2, MBC3 (including RTC), and MBC5 banking
- Persistent battery-backed `.sav` RAM and MBC3 `.rtc` clock state
- Table-driven CPU tests for opcode matrices, timing, flags, PC, stack, and memory effects
- Headless command-line runner
- Headless Mooneye/serial conformance test runner
- SDL desktop local link sessions with two synchronized emulator cores; local
  and TCP link paths are covered by automated protocol and fault tests, and
  manual two-instance Pokémon trades and battles have completed successfully
- Emscripten/WebAssembly browser frontend with IndexedDB cartridge saves
- Android native library/settings dashboard with SDL3 gameplay and multitouch controls
- Shared desktop/Android ROM catalog with fingerprint-deduplicated history and metadata
- Portable desktop `settings.ini` with shareable palette, keyboard, and gamepad mappings
- Game Boy Printer serial protocol with automatic desktop image export
- Game Boy Camera cartridge support with live SDL3 webcam input on desktop
- MBC5 rumble output through compatible SDL3 gamepads on desktop
- Versioned, ROM-validated save states with configurable save/load, fast-forward,
  and rewind controls
- Opt-in logging, link traces, CPU/PPU/APU traces, model-matrix diagnostics,
  and deterministic SGB trace capture/replay ([details](docs/sgb-traces.md))
- Contract and conformance tests that run without proprietary ROMs in the repository

Known limitations:

- Compatibility is still incomplete, especially for untested commercial games
  and revision-specific hardware edge cases.
- The Super Game Boy implementation is a deterministic clean-room adapter,
  not a full SNES emulator or Nintendo BIOS replacement; SNES audio, boot
  animation, fade timing, and the complete boot handshake remain out of scope.
- Web link sessions are not exposed yet. Desktop local/TCP link sessions work,
  but Pokémon can spend a long time in some trade or battle transition states;
  improving that wait-state/audio behavior is still planned.
- The optional hardware-model matrix and AGE/SameSuite research suites expose
  additional reviewed or exploratory results outside the release gate. One
  GBMicrotest DMG case remains a documented upstream expectation mismatch.
- ROMs are not bundled. Only use cartridge dumps and save data you are legally
  entitled to use.

DMG games can use the automatic Game Boy Color compatibility palettes selected
from their cartridge headers.

## Nintendo and third-party intellectual property

Go Bigger Boy is an independent, non-Nintendo project. It is not affiliated
with, endorsed by, sponsored by, or licensed by Nintendo. Nintendo, Game Boy,
Game Boy Color, Super Game Boy, Pokémon, and related names, logos, systems,
games, characters, and other content are trademarks and/or copyrighted works
of Nintendo or their respective owners. See [Nintendo's trademark
information](https://www.nintendo.com/en-gb/Legal-information/Nintendo-s-Intellectual-Property-Enforcement-Program/IP-Enforcement-and-Legal-Frequently-Asked-Questions/What-are-Trademarks-/What-are-Trademarks-732161.html).

The emulator is developed as an independent clean-room implementation. No
Nintendo source code, proprietary SDK code, proprietary boot ROM, game ROM,
artwork, audio, font, or other Nintendo-owned asset was copied into or
distributed with this repository. Emulator behavior is implemented from
publicly available technical research, independent testing, and independently
written code and tests. The repository's diagnostic boot ROM is original
project code and is not a Nintendo boot ROM.

Users must provide their own legally obtained ROMs and save data. No Nintendo
software or game content is bundled, downloaded, or distributed by this
project. This notice describes the project's provenance and is not a guarantee
of legal status in every jurisdiction; users are responsible for complying
with applicable law.

## License

Unless otherwise noted, Go Bigger Boy source code is licensed under the GNU
General Public License, version 3 or later (`GPL-3.0-or-later`); see
[LICENSE](LICENSE) for the complete terms.

ROMs, save files, and other user-provided game data are not covered by this
project license. Third-party libraries, tools, artwork, and metadata retain
their own licenses; the relevant notices and upstream projects are linked
where they are used.

## Build and development

A basic native build and test run is:

```sh
cmake -S . -B build
cmake --build build
ctest --test-dir build --output-on-failure
```

The detailed [build and testing guide](docs/build-and-testing.md) covers
fuzzing, sanitizers, conformance ROMs, hardware-model matrices, SGB traces,
frontend smoke tests, and diagnostic workflows.

See the [desktop frontend guide](docs/desktop.md) for debugger, movie/TAS,
sprite-editing, and GameShark documentation. The [platform guide](docs/platforms.md)
covers Linux, Android, and Web builds.

## Documentation

- [Accuracy and compatibility report](docs/accuracy.md)
- [Build and testing](docs/build-and-testing.md)
- [Desktop frontend and tools](docs/desktop.md)
- [Platform guides](docs/platforms.md)
- [Link cable diagnostics](docs/link-cable.md)
- [Link compatibility matrix](docs/link_compatibility.md)
- [SGB traces and replay](docs/sgb-traces.md)
- [Architecture](docs/architecture.md)
- [Cloud-save synchronization contract](docs/cloud-save-sync.md)
- [Plug-in ABI, manifests, and security](docs/plugin-abi.md)
- [Voxel rendering research](docs/voxel-rendering-research.md)
- [Release checklist](docs/release-checklist.md)

## Automated desktop builds

The `Desktop builds` GitHub Actions workflow builds and tests downloadable
Release artifacts for:

- Windows x64
- Linux x64
- macOS Apple Silicon
- macOS Intel

Open a workflow run's **Artifacts** section to download the archive for your
platform. The Windows and macOS archives include the SDL3 runtime. The Linux
artifact is a self-contained AppImage with SDL3, the desktop launcher, and the
icon. ROM files are never included in CI artifacts.

Pushing a version tag such as `v0.35.21` waits for every platform build to pass,
then automatically creates a GitHub Release with all four platform artifacts and
generated release notes. Tagged builds derive their displayed version from the
tag so the startup update comparison remains accurate. A failed platform build
prevents the release.

The [release checklist](docs/release-checklist.md) covers the manual link
session smoke test and platform packaging checks to repeat before publishing.

Windows SmartScreen may warn when an executable downloaded from GitHub has no
trusted publisher signature. The release workflow supports Authenticode
signing when the repository maintainer configures the encrypted
`WINDOWS_SIGNING_CERTIFICATE_BASE64` (base64-encoded `.pfx`) and
`WINDOWS_SIGNING_CERTIFICATE_PASSWORD` secrets. A publicly trusted code-signing
certificate is required; self-signing the executable will not remove the
warning for other users.

## Layout

The project provides a system-neutral core registry so future systems such as
Game Boy Advance can be added as separate cores. The browser and CLI already
consume this boundary; the SDL shell migration is tracked explicitly. See the
[multi-core architecture](docs/architecture.md) for the boundary and extension
steps.

The [cloud-save synchronization contract](docs/cloud-save-sync.md) describes
the provider-neutral manifest and conflict policy planned for optional
cross-device saves.

```text
include/gbb/      System-neutral frontend/core, scene, and registry APIs
include/gameboy/  GB/GBC core API
src/              Core implementations and adapters
apps/cli/         Headless development frontend
apps/test_runner/ Conformance ROM runner
apps/web/         Emscripten browser frontend
web/              Browser page shell
tests/            Core unit tests
```
