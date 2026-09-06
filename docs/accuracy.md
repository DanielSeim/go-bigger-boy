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
| Blargg | 38/38 | CPU/timing baseline plus all 12 DMG and all 12 CGB sound ROMs |
| Visual PPU | 21/21 | Acid2, Scribbltests, Mealybug, and Gambatte framebuffer comparisons |
| Total CI gate | **168** | Every listed ROM must pass before a release can be published |

The acceptance figure covers every acceptance ROM in the pinned bundle. Tests with
mutually exclusive boot-ROM expectations run under explicit DMG0, DMG/MGB,
SGB, SGB2, CGB0, or CGB post-boot hardware profiles. Mooneye's two AGB-only
misc ROMs are excluded because GBB does not emulate Game Boy Advance hardware.

## Super Game Boy baseline

Cartridges with the SGB header capability flag (`0x0146 = 0x03`) are selected
automatically for the SGB hardware profile unless they require CGB hardware.
The SGB path is a deterministic HLE implementation: JOYP command packets are
decoded at the bit level, `PAL01`/`PAL23`/`PAL03`/`PAL12` set RGB555 palettes,
and `ATTR_BLK`/`ATTR_LIN`/`ATTR_DIV`/`ATTR_CHR` update the 20×18 tile attribute
map used by the Game Boy viewport. `CHR_TRN` and `PCT_TRN` now snapshot their
4 KiB VRAM payloads into SNES-side transfer latches, and `MASK_EN` implements
disabled, freeze, black, and color-zero viewport modes. These latches and the
packet parser are covered by core tests and save states (version 23). This is
deliberately not a full SNES emulation path: the real adapter relies on SNES-
side execution, graphics, and audio ([Pan Docs SGB overview](https://gbdev.io/pandocs/SGB_Functions.html)), so complete SGB compatibility would
require either those SNES subsystems or an equivalent dedicated host model.
A future frontend phase can consume the retained transfer data to compose the
full 256×224 SNES border; SNES audio, multiplayer polling, fade timing, and
the complete SGB boot/header handshake remain deferred. These limitations do
not affect ordinary DMG or CGB emulation.

The APU evaluates channel output and the hardware high-pass response on every
master-clock cycle, then integrates those values over exact 48 kHz sample
boundaries. This preserves short duty/noise transitions that a boundary sampler
would discard while keeping the public frontend format at stereo 16-bit PCM.
Core tests also retain deterministic, quantized PCM signatures for representative
pulse, wave, and noise fixtures, so changes to channel timing or mixer output
cannot silently alter the generated waveform. Desktop audio cleanup clears an
overdue queue even when a frame produces no new samples, preventing stale audio
from repeating after a pause or link wait.

Serial transfers now retain the divider phase and partial-byte state in
save-state version 24. This matters for CGB fast mode because the serial clock
uses a 16-cycle sub-period; restoring only SB/SC could otherwise move the next
edge or replay an entire byte at once. Older save-state versions remain
loadable with the previous restart-at-boundary behavior. CGB SCY's pending
two-T-cycle latch is preserved by save-state version 25 for the same reason:
restoring during a mode-3 write must not expose the new scroll value early.

## Accuracy status

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
`SCX` scrolling is active. The `WX=6` handoff is covered by the
`m3_wx_6_change` visual fixture: near-edge rewrites preserve the already
primed background prefix while advancing the window FIFO, and retargets at or
within the four-dot handoff latency miss the activation for that line. The
`WX=4`, `WX=5`, and `WX=6` Mealybug references now match exactly.
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

DIV/APU edges are dispatched at their timer-cycle boundary rather than being
queued until the end of a bus batch. The APU also models the hardware rule that
enabling it while the DIV/APU input is high skips the first falling-edge event;
the pending phase is retained in save states. This is covered by the core timer
tests and the corresponding SameSuite DIV-trigger ROMs. On modern CGB starts,
inactive square channels also retain the additional long-period alignment
interval observed by SameSuite's `channel_[12]_volume_div` cases, while active
restarts keep the established startup timing.

Square duty writes now use a latched duty value: writes made while a channel is
active take effect at the next waveform boundary, while writes made while it is
inactive are applied for the next trigger. The latched and pending values are
stored in save-state version 19, and trigger suppression plus the CGB 1 MHz
phase are stored in version 20, so restoring during a duty transition, period
reload, or speed-switch half-cycle remains deterministic. The APU phase is
stable during normal-speed execution and advances only on alternating CGB
double-speed half-cycles; this also works when bus ticks are split across
individual CPU accesses.

Frequency writes that land on a waveform reload now update the active period at
that boundary; otherwise the current countdown is preserved. Revision-specific
restart and envelope-glitch behavior remains tracked separately, while the
double-speed 1 MHz phase boundary is now covered by the CGB contract tests.
The current implementation preserves the existing audio regressions while
covering the stable startup, divider, reload-boundary, and speed-switch timing
cases.

The mode-3 timing model now carries the hardware distinction needed by these
edge cases: DMG and early CGB revisions re-sample SCY during both bitplane
fetches, while CPU GBC D and later use the tile-name sample only; CGB writes
also pass through the documented two-T-cycle latch. The in-flight SCY latch is
serialized so a restore cannot change a partially fetched tile. Object
cancellation now invalidates both the queued pixel and its cancellation
deadline, preventing stale output-pipeline state from leaking into a later
LCDC transition. The remaining revision-specific framebuffer references are
kept as diagnostic fixtures until the CPU C/D profiles are fully matched
pixel-for-pixel.

CGB HBlank DMA now consumes each HBlank edge at dot granularity. Previously a
large peripheral tick could collapse multiple PPU HBlank notifications into one
bitmask, causing one or more requested 16-byte blocks to be skipped. The core
regression suite covers a batched tick that crosses two HBlanks and verifies that
both blocks are copied while the transfer remains active for the requested count.

Window comparator positions are normalized at the visible left edge for
`WX<7`, including writes made while the current window tile is still queued.
The core suite now exercises every `WX=1..6` value with a distinct first-tile
column pattern, guarding the visible-edge comparator and source-column mapping
independently of the external framebuffer fixtures.
The `WX=6` handoff is covered by the exact Mealybug reference in the visual
release gate; the core `WX=1..6` loop independently checks the visible-edge
source-column comparator for every off-screen-left value.

For timing investigations, set `GBB_TRACE_WX` to a file path before running an
emulator or test runner (or set it to `1`/`stderr` to write to standard error).
The opt-in trace records each visible mode-3 dot together with `LY`, `WX`,
output position, window source position, fetch state, activation/retrigger
state, and window cancellation/resume events. It is disabled by default and
has no output or file side effects unless the variable is set.

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
