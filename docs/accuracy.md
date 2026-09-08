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
| GBMicrotest | matrix-only | HRAM self-checking cycle-accuracy ROMs from the pinned v7.0 bundle |
| Mooneye-wilbertpol | matrix-only | Extended Mooneye acceptance/misc/emulator-only ROMs (manual-only cases excluded) |
| AGE and SameSuite | deferred | Screenshot/interactive and diagnostic cases awaiting suite-specific harnesses |
| Total release gate | **168** | The reviewed fixed baseline must pass before a release can be published |

The acceptance figure covers every acceptance ROM in the pinned bundle. Tests with
mutually exclusive boot-ROM expectations run under explicit DMG0, DMG/MGB,
SGB, SGB2, CGB0, CGB-C, or CGB-E post-boot hardware profiles. Mooneye's two AGB-only
misc ROMs are excluded because GBB does not emulate Game Boy Advance hardware.

### Additional pinned-bundle suites

The v7.0 archive is also treated as a source of matrix cases rather than only a
collection of hand-maintained paths. The matrix runner discovers every
machine-readable `.gb` in the supported suite directories below, so a new ROM
cannot be silently omitted from the opt-in matrix. Upstream helper/manual
directories are intentionally excluded, as are GBMicrotest power-on fixtures
until the emulator can run an actual boot ROM; those images do not implement
the result protocol from a post-boot run. Mooneye `boot_*` and `boot-*` images
are excluded for the same reason. Individual discovered CTest cases
are available only with `-DGAMEBOY_ENABLE_DISCOVERED_CONFORMANCE=ON` and are
not part of the normal release gate:

* **GBMicrotest** reports an observed byte, expected byte, and completion flag
  in HRAM (`FF80`, `FF81`, and `FF82`). The headless runner exits immediately
  with a diagnostic pass/fail result.
* **Mooneye-wilbertpol** uses the Fibonacci register result values with its
  historical `0xED` completion opcode. Only `acceptance`, `emulator-only`, and
  `misc` are machine-readable; helper, `manual-only`, and boot-only diagnostics
  remain outside this post-boot matrix until their required harness exists.

AGE and SameSuite are deliberately not discovered by the matrix yet. AGE is
primarily screenshot-driven, while SameSuite mixes interactive diagnostics and
revision-specific APU experiments; neither has a verified machine-readable
completion contract for the headless runner. They will be added only after a
dedicated harness can capture their required frames/input and classify results
without manufacturing `REGRESSION` outcomes.

The upstream collection also contains visual or interactive suites (AGE
screenshots, Bully, cgb-acid-hell, MBC3 Tester, rtc3test, TurtleTests, and
parts of little-things-gb). Their ROMs and references remain documented by the
bundle, but they are not registered as pass/fail CTest cases yet: each needs
suite-specific frame timing, input scripting, or screenshot selection. This
distinction prevents a timeout or an unreviewed screenshot from being reported
as a core emulation failure.

The `gameboy_hardware_model_matrix_contract` test complements the ROM suites by
constructing each supported profile directly. It checks the post-boot CPU
registers, DIV handoff value, serial-divider phase, JOYP selection, APU channel
startup state, CGB register defaults, and save-state round trips. Automatic
selection is also checked for ordinary DMG, SGB-capable, and CGB-capable
cartridges. This is a digital profile contract; it does not claim to model
analog clock tolerance, LCD response, DAC variation, or Game Boy Advance/Game
Boy Player hardware.

The revision matrix is intentionally explicit: `dmg0`, `dmg` (DMG-B), `mgb`,
`sgb`, `sgb2`, `cgb0`, `cgb-c`, and `cgb-e` are selectable in the test runner. The historical
`cgb` spelling remains an alias for `cgb-e` so existing scripts and save states
keep their behavior. The CGB-C profile uses the early-revision APU and DIV
phase rules, while CGB-E enables the late-revision envelope (NRx2 zombie-mode)
behavior. The focused APU contract covers the reviewed CGB-C/CGB-E envelope
write mismatch; channel alignment, PCM register visibility, and noise LFSR
startup remain covered by shared contracts until a reproducible revision-only
mismatch is measured.

The Wilbert `acceptance/timer/timer_if.gb` fixture is a documented exception:
upstream reports results only for MGB, CGB, and AGS. The matrix therefore marks
DMG and SGB profiles as `EXPECTED_FAIL` (outside the fixture's tested set), while
the currently reproducible MGB/CGB one-M-cycle normal-interrupt TIMA boundary is
listed as `KNOWN_FAIL` in `tests/model_expectations.json`. The core retains the
verified 20-cycle interrupt dispatch contract and the complete timer unit suite;
this narrow boundary remains an explicit accuracy target rather than a release
gate regression.

The same expectations file records the per-ROM `Verified results` scopes from
the pinned Wilbert source for the reviewed GPU timing fixtures. A fixture marked
`pass: MGB` is therefore only applicable to MGB, while `pass: CGB` covers the
represented CGB-0/CGB-C/CGB-E profiles. Profiles not listed by upstream are
reported as `EXPECTED_FAIL`; a failure inside a listed scope remains a genuine
`REGRESSION` candidate. This keeps the matrix useful for debugging without
turning upstream's intentionally untested hardware into false failures.

For a full ROM-by-model run, configure with
`-DGAMEBOY_ENABLE_MODEL_MATRIX=ON` and execute `ctest -L model-matrix`. The
`hardware_model_matrix_report` test writes a Markdown table containing one
row per ROM and model. Each row is classified as `PASS`, `EXPECTED_FAIL` (the
ROM's documented hardware group excludes that model), `KNOWN_FAIL` (a reviewed
limitation listed in `tests/model_expectations.json`), or `REGRESSION` (an
unexpected failure). A ROM can therefore be reported in the compact form
`same-suite/foo.gb CGB-C -> EXPECTED_FAIL; CGB-E -> PASS` without hiding the
hardware-specific result. The matrix is opt-in because running every
deterministic ROM eight times is substantially slower than the normal release
gate.

CI runs the same matrix in the dedicated [Hardware model matrix workflow](https://github.com/DanielSeim/go-bigger-boy/actions/workflows/hardware-model-matrix.yml).
Its run summary shows aggregate counts, and the `hardware-model-matrix`
artifact contains the complete report. Matrix findings are intentionally kept
separate from the Desktop builds workflow while applicability and emulator
revision mismatches are being reviewed.

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
baseline files beside `gameboy_apu_waveform_contract_tests`, so the same check
works from a Windows build or an arbitrary working directory.

For comparison against a recording from hardware or a trusted emulator, place
matching `dmg-pulse.txt`, `dmg-wave.txt`, `dmg-noise.txt`, `cgb-pulse.txt`,
`cgb-wave.txt`, and `cgb-noise.txt` files in
[`tests/fixtures/audio-external`](../tests/fixtures/audio-external) (or another
separate directory) and run the focused waveform contract:

```sh
GBB_AUDIO_REFERENCE_DIR=/path/to/reference \
GBB_AUDIO_REQUIRE_EXTERNAL=1 \
  build-sdl/gameboy_apu_waveform_contract_tests
```

Each file uses the `GBB audio waveform reference v1` text format. Its metadata
declares the sample rate, channel count, quantization, sample count, source,
comparison mode, and allowed `max_abs_error`/`rms_error`; the test reports the
first differing
sample when a fixture exceeds either limit. External files must include
`source=hardware` or `source=trusted-emulator` (the older `source=external`
spelling remains accepted for compatibility) when the strict external gate is
enabled. To capture the emulator's current output for inspection (not to
replace a trusted reference), use:

```sh
GBB_AUDIO_REFERENCE_CAPTURE_DIR=/tmp/gbb-audio-reference \
GBB_AUDIO_REFERENCE_DIR=/tmp/gbb-audio-reference \
  build-sdl/gameboy_apu_waveform_contract_tests
```

The conversion and tolerance workflow is provided by
[`scripts/audio_reference.py`](../scripts/audio_reference.py). It accepts
uncompressed 48 kHz, 16-bit stereo WAV by default so a bad capture setup
cannot be hidden by an implicit resample or channel conversion. SameBoy's
commonly used 96 kHz output is supported only with an explicit
`--downsample 2`; the converter applies a deterministic two-frame box average
before quantization. Use `--source trusted-emulator` and record the exact
SameBoy commit, model/revision, callback rate, fixture, alignment, and gain in
`--provenance`. Hardware captures use `--source hardware`. Capture at least
three takes, convert each one, and use `aggregate` to build a per-sample
median reference; never mix the two source classes. The aggregate reports
observed variation and writes `max_abs_error`/`rms_error` with a one-quantum
safety margin by default; use `--margin` to make that review decision explicit.
The complete procedure and provenance requirements are in
[`tests/fixtures/audio-external/README.md`](../tests/fixtures/audio-external/README.md).
The SameBoy release and immutable commit used by this workflow are pinned in
[`sameboy-reference-pin.json`](../tests/fixtures/audio-external/sameboy-reference-pin.json).

The release accuracy workflow runs the external gate automatically whenever
reviewed `*.txt` files are present in that directory. Until captures are
reviewed, CI continues using the deterministic software fixtures. SameBoy is a
trusted digital reference only; it does not validate hardware DAC levels,
analog high-pass response, amplifier noise, or LCD/audio coupling. The
official SameBoy project documents revision-specific models and sample-
accurate audio output at [`sameboy.github.io/features`](https://sameboy.github.io/features/)
and exposes sample-rate/audio-recording APIs in its
[`Core API`](https://github.com/LIJI32/SameBoy/wiki).

The pinned SameBoy fixture producer and three-take captures have been exercised
locally. Direct PCM comparison shows a substantial mixer level/filter and
startup-window difference from GBB (SameBoy uses band-limited digital
synthesis), so the reviewed SameBoy files use `comparison=normalized`. The
normalized comparator removes each channel's DC mean and RMS scale after the
explicit 96 -> 48 kHz conversion; it remains sensitive to timing, duty-cycle,
wavetable, and LFSR shape while deliberately not claiming DAC gain or analog
filter agreement. The six reviewed references align a 16-frame startup trim
and use a three-unit normalized tolerance margin. Raw hardware captures continue to
use the default `comparison=raw` path for absolute-level validation.

The revision-by-revision capture review (including the pinned-model limitations
and the reasons no insensitive startup fixtures were promoted) is recorded in
[`sameboy-revision-matrix.md`](../tests/fixtures/audio-external/sameboy-revision-matrix.md).

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
