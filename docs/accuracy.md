# Accuracy status

The automated baseline uses the pinned
[`c-sp/game-boy-test-roms` v7.0](https://github.com/c-sp/game-boy-test-roms/releases/tag/v7.0)
bundle. GitHub Actions verifies the archive checksum before running any ROM.

## Current automated baseline

| Suite | Passing | Coverage |
| --- | ---: | --- |
| Mooneye acceptance | 75/75 | Complete acceptance directory, with model-specific boot profiles |
| Mooneye CGB misc | 6/6 | Every CGB/CGB0 ROM applicable to emulated Game Boy Color hardware |
| Mooneye emulator-only | 28/28 | Complete MBC1, MBC2, and MBC5 mapper directories |
| Blargg | 14 | All 11 individual CPU instruction ROMs plus instruction and memory timing |
| Visual PPU | 15/15 | Acid2, Scribbltests, Mealybug, and Gambatte framebuffer comparisons |
| Total CI gate | **138** | Every listed ROM must pass before a release can be published |

The acceptance figure covers every acceptance ROM in the pinned bundle. Tests with
mutually exclusive boot-ROM expectations run under explicit DMG0, DMG/MGB,
SGB, SGB2, CGB0, or CGB post-boot hardware profiles. Mooneye's two AGB-only
misc ROMs are excluded because GBB does not emulate Game Boy Advance hardware.

## Next accuracy work

The visual harness runs a ROM to a deterministic frame, writes a dependency-free
PPM capture, and compares every RGB pixel with the suite's reference PNG. A
failure keeps both the captured frame and a magenta difference image under the
build directory's `visual-results` folder.

The PPU now emits pixels incrementally during mode 3 instead of rendering an
entire line at the mode-0 boundary. The visual gate covers native DMG and CGB
Acid2, DMG software under the CGB compatibility palette, Scribbl rendering and
STAT timing, a Mealybug window case captured at its `LD B,B` breakpoint, and two
Gambatte mid-scanline palette cases.

The next PPU milestone is replacing the incremental renderer's direct tile
lookups with a fetcher/FIFO pipeline. That is required for the remaining
Mealybug and Gambatte cases involving fetch latency, sprite stalls, and
mid-scanline SCX/SCY/LCDC changes.

Run the exact CI baseline locally with:

```sh
cmake -S . -B build-conformance \
  -DGAMEBOY_BUILD_SDL=OFF \
  -DGAMEBOY_TEST_ROM_DIR=/path/to/game-boy-test-roms-v7.0
cmake --build build-conformance
ctest --test-dir build-conformance -L conformance --output-on-failure
```

Run only the exact framebuffer comparisons with:

```sh
ctest --test-dir build-conformance -L visual --output-on-failure
```
