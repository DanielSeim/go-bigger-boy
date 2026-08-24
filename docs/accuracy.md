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
| Visual PPU | 20/20 | Acid2, Scribbltests, Mealybug, and Gambatte framebuffer comparisons |
| Total CI gate | **143** | Every listed ROM must pass before a release can be published |

The acceptance figure covers every acceptance ROM in the pinned bundle. Tests with
mutually exclusive boot-ROM expectations run under explicit DMG0, DMG/MGB,
SGB, SGB2, CGB0, or CGB post-boot hardware profiles. Mooneye's two AGB-only
misc ROMs are excluded because GBB does not emulate Game Boy Advance hardware.

## Next accuracy work

The visual harness runs a ROM to a deterministic frame, writes a dependency-free
PPM capture, and compares every RGB pixel with the suite's reference PNG. A
failure keeps both the captured frame and a magenta difference image under the
build directory's `visual-results` folder.

The PPU emits pixels through a dot-stepped fetcher instead of looking up a tile
directly for each output pixel. The fetcher performs tile, low-bitplane,
high-bitplane, sleep, and push phases; feeds a 16-pixel background FIFO; applies
the initial `SCX` discard; restarts for the window; and pauses output for
selected sprite fetches. Sprite pixels are latched into a scanline buffer and
mixed with background pixels as the FIFO is popped. Save-state version 9 also
preserves this in-flight pipeline so a state taken during mode 3 resumes
deterministically.

The visual gate covers native DMG and CGB Acid2, DMG software under the CGB
compatibility palette, Scribbl rendering and STAT timing, six Mealybug window
and WX cases captured at their `LD B,B` breakpoints, and two Gambatte
mid-scanline palette cases. The window pipeline now inserts the reactivation
color-zero pixel without consuming the queued window pixel, latches disable
requests through the next tile boundary, resumes background fetching, and can
restart on a later WX position using the next internal window row.
If a following WX write cancels a just-emitted reactivation pixel, the FIFO now
realigns without dropping the first queued window pixel.
DMG window-enable changes also latch a comparator already inside the queued
tile, cancel fetches during their first two pixels, insert the color-zero pixel
at the tile-name boundary, and delay off-screen-left disables through the next
full visible tile.

The next PPU refinement is the hardware-revision-specific edge behavior around
object-fetch cancellation, window triggers changed before the first visible
pixel, `WX < 7`, and bitplane reads that overlap precisely timed SCY/LCDC
writes. Those cases remain outside the release gate until their framebuffer
references match exactly.

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
