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
| Blargg | 38 | CPU/timing baseline plus all 12 DMG and all 12 CGB sound ROMs |
| Visual PPU | 20/20 | Acid2, Scribbltests, Mealybug, and Gambatte framebuffer comparisons |
| Total CI gate | **167** | Every listed ROM must pass before a release can be published |

The acceptance figure covers every acceptance ROM in the pinned bundle. Tests with
mutually exclusive boot-ROM expectations run under explicit DMG0, DMG/MGB,
SGB, SGB2, CGB0, or CGB post-boot hardware profiles. Mooneye's two AGB-only
misc ROMs are excluded because GBB does not emulate Game Boy Advance hardware.

The APU evaluates channel output and the hardware high-pass response on every
master-clock cycle, then integrates those values over exact 48 kHz sample
boundaries. This preserves short duty/noise transitions that a boundary sampler
would discard while keeping the public frontend format at stereo 16-bit PCM.
Core tests also retain deterministic, quantized PCM signatures for representative
pulse, wave, and noise fixtures, so changes to channel timing or mixer output
cannot silently alter the generated waveform. Desktop audio cleanup clears an
overdue queue even when a frame produces no new samples, preventing stale audio
from repeating after a pause or link wait.

## Next accuracy work

The visual harness runs a ROM to a deterministic frame, writes a dependency-free
PPM capture, and compares every RGB pixel with the suite's reference PNG. A
failure keeps both the captured frame and a magenta difference image under the
build directory's `visual-results` folder.

The PPU emits pixels through a dot-stepped fetcher instead of looking up a tile
directly for each output pixel. The fetcher performs tile, low-bitplane,
high-bitplane, sleep, and push phases; feeds a 16-pixel background FIFO; applies
the initial `SCX` discard; restarts for the window; and pauses output for
selected sprite fetches. The first mode-3 tile fetch is discarded and tile zero
is fetched again, matching the hardware pipeline's observable register-sampling
sequence rather than reusing the visually identical speculative data. Tile-map
and tile bitplane data are read at their individual fetcher bus phases, including
the delayed high-bitplane read while the FIFO is blocked. Sprite pixels are
latched into a scanline buffer and mixed with background pixels as the FIFO is
popped. Sprite height is latched with the line's OAM selection, so mid-scanline
`LCDC.OBJ_SIZE` writes no longer change sprites that have already been selected.
Each queued object fetch tracks both pixel availability and its later hardware
cancellation boundary. Pixel-level deadlines model the object FIFO draining
over successive dots, so a mid-fetch OBJ disable can preserve the completed
prefix while cancelling only the unfinished tail;
the aborted handoff also contributes its eight-dot PPU stall. The two-phase
fetch queue and latched sprite height are preserved by save-state version 15,
so a state taken during mode 3 resumes deterministically. Disabling OBJ also
restores already-emitted pixels from a cancelled fetch to the exact background
sample that was present when the pixel left the FIFO. These per-pixel samples
are preserved by save-state version 17, keeping mid-scanline restores
deterministic when LCDC changes after a state is loaded. Object-pixel
cancellation deadlines are serialized in save-state version 18, so a restore
cannot change which suffix of a sprite remains visible. The
two partially off-screen DMG object positions (X=3 and X=4) use their shorter
four-dot handoff tail rather than the general cancellation window.

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
full visible tile. `WX=0` now also pays its extra DMG activation dot when fine
`SCX` scrolling is active. An early rewrite from `WX=6` to a lower comparator
now defers the active handoff through the queued window-tile boundary before
resuming the background fetch path, preserving the queued tile boundary on
reactivation, and reducing the exploratory `m3_wx_6_change` mismatch from
13,810 to 12,932 pixels without changing the existing `WX=4/5` references.
Later monochrome post-boot profiles reproduce the
registered-trademark tile that the boot ROM leaves at `$8190`, so edge tests do
not accidentally run against zero-filled startup VRAM.

The audio tests include explicit 48 kHz stereo waveform vectors for pulse, wave,
and noise fixtures on both DMG and CGB models under
[`tests/fixtures/audio`](../tests/fixtures/audio). The
vectors are quantized to one unit per 64 PCM levels so harmless low-bit
floating-point rounding does not make Linux and Windows disagree. They are the
reviewable software baseline; the existing Blargg sound ROMs still validate
CPU-visible APU behavior rather than analog PCM output. CMake copies the
baseline files beside `gameboy_tests`, so the same check works from a Windows
build or an arbitrary working directory.

For comparison against a recording from hardware or a trusted emulator, place
matching `dmg-pulse.txt`, `dmg-wave.txt`, `dmg-noise.txt`, `cgb-pulse.txt`,
`cgb-wave.txt`, and `cgb-noise.txt` files in a separate directory and point the
unit test at it:

```sh
GBB_AUDIO_REFERENCE_DIR=/path/to/reference build-sdl/gameboy_tests
```

Each file uses the `GBB audio waveform reference v1` text format. Its metadata
declares the sample rate, channel count, quantization, sample count, and allowed
`max_abs_error`/`rms_error`; the test reports the first differing sample when a
fixture exceeds either limit. To capture the emulator's current output for
inspection (not to replace a trusted reference), use:

```sh
GBB_AUDIO_REFERENCE_CAPTURE_DIR=/tmp/gbb-audio-reference \
GBB_AUDIO_REFERENCE_DIR=/tmp/gbb-audio-reference \
  build-sdl/gameboy_tests
```

The next accuracy pass is to collect those same fixtures from DMG and CGB
hardware (or a validated hardware-level emulator), set tolerances from the
measurement noise, and then keep those external references in the release
verification job. That will cover revision-specific DAC levels and analog
high-pass response without weakening the deterministic software regression.

SameSuite provides the complementary digital-APU research tests. It is an
opt-in CTest suite because its APU ROMs intentionally expose revision-specific
edge cases that are still exploratory even in established emulators. Build a
checkout with RGBDS, then configure GBB with
`-DGAMEBOY_SAMESUITE_DIR=/path/to/SameSuite`; run the suite with:

```sh
ctest --test-dir build-conformance -L samesuite-apu --output-on-failure
```

The runner uses SameSuite's Mooneye-compatible result registers and selects DMG
for its two pre-CGB DIV-trigger ROMs, CGB0 for explicitly tagged CGB0 cases,
and CGB for the remaining CGB tests. This suite is diagnostic until its
revision-specific failures are resolved; it is not included in the release
gate by default.

The next PPU refinement is the hardware-revision-specific edge behavior around
object-fetch cancellation, window triggers changed before the first visible
pixel (including the remaining `WX=1..6` comparator cases), and the output-pipeline
collisions caused by precisely timed SCY/LCDC writes. Those cases remain outside
the release gate until their framebuffer references match exactly.

Window comparator positions are normalized at the visible left edge for
`WX<7`, including writes made while the current window tile is still queued.
The `WX=6` sequence still has a substantial mismatch after the handoff because
the internal window row and queued tile pipeline are not yet fully modeled
cycle by cycle; it remains exploratory and is not part of the release gate.

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
