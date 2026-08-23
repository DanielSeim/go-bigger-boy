# Go Bigger Boy (GBB)

A portable, dependency-free C++17 Game Boy emulator. Platform frontends
(desktop, Android, and Switch) will live outside the core so emulation logic is
shared everywhere.

## Current status

- Cartridge loading and basic header parsing
- Initial DMG memory map, including work RAM echo behavior
- Complete legal CPU opcode set, including all 256 CB-prefixed operations
- Interrupt dispatch, EI delay, HALT/STOP states, HALT bug, and machine-cycle bus timing
- Cycle-driven DIV/TIMA/TMA/TAC timer with overflow interrupts and write-edge behavior
- DMG PPU modes, LCD/STAT interrupts, VRAM/OAM arbitration, and RGBA framebuffer
- Background, window, and 8×8/8×16 sprite scanline rendering
- Active-low joypad matrix with keyboard/gamepad input and interrupts
- Cycle-timed OAM DMA with source-bus conflicts and an optional SDL3 desktop frontend
- Four-channel DMG audio with 48 kHz stereo SDL3 playback
- ROM-only, MBC1, MBC3 (including RTC), and MBC5 banking
- Persistent battery-backed `.sav` RAM and MBC3 `.rtc` clock state
- Table-driven CPU tests for opcode matrices, timing, flags, PC, stack, and memory effects
- Headless command-line runner
- Headless Mooneye/serial conformance test runner
- Emscripten/WebAssembly browser frontend with IndexedDB cartridge saves
- Versioned, ROM-validated save states with desktop quick-save controls
- Dependency-free unit tests

This is an early emulator with incomplete game compatibility.

## Build

```sh
cmake -S . -B build
cmake --build build
ctest --test-dir build --output-on-failure
```

Inspect a ROM and execute a requested number of starter instructions:

```sh
./build/gbb_cli path/to/game.gb 10
```

Run an individual acceptance-test ROM with a bounded cycle budget:

```sh
./build/gbb_test_runner path/to/test.gb --max-cycles 100000000
```

The runner recognizes Mooneye's `LD B,B` result protocol and serial test output
containing `Passed` or `Failed`, plus Blargg's `$A000` memory result protocol.
Use `--protocol mooneye`, `--protocol serial`, or `--protocol blargg` to disable
automatic protocol detection. CMake selects the Blargg protocol automatically
for ROMs inside `dmg_sound` and `cgb_sound` directories.

The DMG APU passes all 12 upstream Blargg `dmg_sound` tests, including active
wave-RAM reads/writes and the original hardware's channel 3 retrigger corruption.

To register a directory of legally obtained `.gb` test ROMs with CTest, set
the opt-in cache path when configuring. Test ROMs are deliberately not bundled
or downloaded by the build:

```sh
cmake -S . -B build-conformance \
  -DGAMEBOY_TEST_ROM_DIR=/path/to/mooneye-test-suite/build
cmake --build build-conformance
ctest --test-dir build-conformance -L conformance --output-on-failure
```

When SDL3 is installed, CMake also builds the desktop frontend. Launching it
without arguments opens a native ROM picker:

```sh
./build/gbb
```

You can also pass a ROM path, press Ctrl+O to choose another ROM, or drag a
`.gb`/`.gbc` file onto the emulator window. On Windows, build with Visual
Studio and an SDL3 installation visible to CMake:

```powershell
cmake -S . -B build-windows -G "Visual Studio 17 2022" -A x64 `
  -DCMAKE_PREFIX_PATH=C:\path\to\SDL3
cmake --build build-windows --config Release
.\build-windows\Release\gbb.exe
```

When using SDL3's shared package, the build also copies its runtime DLL beside
the executable.

### Linux desktop

Install a C++ compiler, CMake, SDL3 development files, and an XDG desktop
portal. On Debian/Ubuntu distributions where SDL3 packages are available:

```sh
sudo apt install build-essential cmake libsdl3-dev xdg-desktop-portal
cmake -S . -B build-linux -DCMAKE_BUILD_TYPE=Release
cmake --build build-linux --parallel
./build-linux/gbb
```

For a per-user installation with an application-menu entry and ROM file
association:

```sh
cmake --install build-linux --prefix "$HOME/.local"
```

The installed desktop entry accepts `.gb` and `.gbc` files from Linux file
managers. The native ROM picker uses SDL's portal-backed file dialog on
Wayland and supported X11 desktops.

Keyboard controls are arrows for the D-pad, X for A, Z for B, Enter for
Start, Backspace for Select, and Escape to quit. Standard gamepads are also
supported. Desktop shortcuts are Space to pause, Ctrl+R to reset, F11 for
fullscreen, Ctrl+L for the recent-ROM list, Ctrl+1 through Ctrl+9 for quick
recent selection, Ctrl+K to configure and persist controls, Ctrl+P to choose
between Grayscale, Classic green, Game Boy Pocket, and Amber display palettes,
F5 to quick-save, F8 to load the quick save, and F1 for help. Recent ROMs,
keyboard and gamepad bindings, the display palette, and per-ROM quick saves are
stored in SDL's per-user preferences directory. The Ctrl+K controls dialog can
rebind either input device or restore the default mappings; Escape cancels an
in-progress setup.

Battery-backed MBC1, MBC3, and MBC5 games use a sibling file with the ROM's
base name and a `.sav` extension. MBC3 real-time clocks use an additional
`.rtc` file. MBC5 rumble register state is exposed by the core for frontends;
physical rumble output is not connected yet. Unsupported cartridge controllers
are rejected explicitly instead of running with incorrect banking.

ROM files are not included. Only use cartridge dumps you are legally entitled
to use.

### Web build

The browser frontend uses SDL3 and Emscripten. It accepts local `.gb` and
`.gbc` files from the picker or by drag and drop; ROM data stays in the browser
and is never uploaded. Keyboard and standard gamepad controls match the desktop
frontend. The display palette selector offers the same four palettes as the
desktop frontend and remembers the selection in browser storage. Battery-backed
RAM and MBC3 clock state are saved automatically in IndexedDB for each ROM.
Browser saves can also be imported from or exported to desktop-compatible
`.sav` and `.rtc` files.

With the Emscripten SDK active and an SDL3 installation built for Emscripten:

```sh
emcmake cmake -S . -B build-web -DCMAKE_BUILD_TYPE=Release \
  -DGAMEBOY_BUILD_TESTS=OFF \
  -DSDL3_DIR=/path/to/emscripten-sdl3/lib/cmake/SDL3
cmake --build build-web --target gameboy_web --parallel
emrun build-web/web/index.html
```

The `Web build and Pages` workflow repeats this build on every push to `main`
and deploys the result to GitHub Pages. Pull requests build the WebAssembly site
without deploying it. The repository's Pages source must be set to **GitHub
Actions** before the first deployment.

## Automated desktop builds

The `Desktop builds` GitHub Actions workflow builds and tests downloadable
Release artifacts for:

- Windows x64
- Linux x64
- macOS Apple Silicon
- macOS Intel

Open a workflow run's **Artifacts** section to download the archive for your
platform. The Windows and macOS archives include the SDL3 runtime. The Linux
archive includes SDL3 alongside the executable plus the desktop launcher and
icon. ROM files are never included in CI artifacts.

Pushing a version tag such as `v0.1.0` waits for every platform build to pass,
then automatically creates a GitHub Release with all four archives and
generated release notes. A failed platform build prevents the release.

## Layout

```text
include/gameboy/  Public core API
src/              Emulator implementation
apps/cli/         Headless development frontend
apps/test_runner/ Conformance ROM runner
apps/web/         Emscripten browser frontend
web/              Browser page shell
tests/            Core unit tests
```
