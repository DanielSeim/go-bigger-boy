# Accuracy status

The automated baseline uses the pinned
[`c-sp/game-boy-test-roms` v7.0](https://github.com/c-sp/game-boy-test-roms/releases/tag/v7.0)
bundle. GitHub Actions verifies the archive checksum before running any ROM.

## Reviewed automated baseline

These are registered release-gate cases, not a fresh test run or a complete
commercial-game compatibility list. Definitions live in
[`ConformanceTests.cmake`](../cmake/ConformanceTests.cmake); current results are
reported by the Desktop builds workflow. The full CTest total varies with
optional suites, local firmware/reference files and frontend dependencies.

| Suite | Required cases | Coverage |
| --- | ---: | --- |
| Mooneye acceptance | 75 | Complete acceptance directory, with model-specific boot profiles |
| Mooneye CGB misc | 6 | Reviewed CGB/CGB0 ROMs applicable to emulated Game Boy Color hardware |
| Mooneye emulator-only | 28 | Complete MBC1, MBC2, and MBC5 mapper directories |
| Blargg | 38 | CPU/timing baseline plus all 12 DMG and all 12 CGB sound ROMs |
| Visual PPU | 21 | Acid2, Scribbltests, Mealybug, and Gambatte framebuffer comparisons |
| GBMicrotest | matrix-only | HRAM self-checking cycle-accuracy ROMs from the pinned v7.0 bundle |
| Mooneye-wilbertpol | matrix-only | Extended Mooneye acceptance/misc/emulator-only ROMs (manual-only cases excluded) |
| AGE, SameSuite and BullyGB | research report | AGE screenshots, SameSuite diagnostics and pinned BullyGB aggregate results, outside the release gate |
| Total release gate | **168** | The reviewed fixed baseline must pass before a release can be published |

The acceptance figure covers every acceptance ROM in the pinned bundle. Tests with
mutually exclusive boot-ROM expectations run under explicit DMG0, DMG/MGB,
SGB, SGB2, CGB0, CGB-C, or CGB-E post-boot hardware profiles. Mooneye's two AGB-only
misc ROMs remain excluded from this silicon-accuracy gate. The selectable
`agb0`/`agb` profiles provide GB/GBC-on-GBA startup compatibility using the
CGB-E runtime baseline, not native GBA emulation or complete AGB silicon accuracy.

### Diagnostic boot path

GBB normally starts cartridges from the established post-boot hardware profile.
The test runner's `--diagnostic-boot` option enables an original, in-tree 256-byte
diagnostic boot ROM instead. It sets the model-specific CPU handoff registers,
writes the `GBB` marker and model ID to HRAM (`FF80`–`FF84`), disables the mapped
ROM through `FF50`, and hands control to the cartridge at `0100`. This path is
intentionally opt-in and does not replace the production startup path. It is a
boot-ROM mapping and handoff harness, not a claim of compatibility with
Nintendo's proprietary boot ROM; hardware initialization remains provided by the
existing model profile in this diagnostic path. Separate original replacement
firmware now performs cold startup for the supported models.

The separate `--dmg-boot --model dmg` option now runs the original DMG
replacement firmware from a deterministic cold machine, without post-boot
register or VRAM seeding. This opt-in fast-start implementation validates the
header checksum and establishes the DMG register handoff, but does not reproduce
Nintendo logo/trademark tiles or complete physical reset equivalence.
Frontend settings expose `instant` (default), `replacement` and `animated`;
the same hardware-neutral modes choose bundled firmware for DMG0, DMG,
Pocket, CGB/CGB0, AGB/AGB0 startup profiles, SGB and SGB2. Animated monochrome
and color boots use original GBB lettering and an APU-synthesized chime;
SGB/SGB2 use their header bootstrap without that intro. Old DMG-specific
setting IDs are not migrated and fall back to instant startup.
See [replacement firmware](../firmware/README.md) for its scope, reproducible
build and state/reset contracts, and [DMG boot validation](dmg-boot-validation.md)
for the execution-only local comparison, verified LCD fixes and remaining
timer/audio differences. It is not used to
inflate the existing post-boot conformance results.

### BullyGB follow-up regression comparison

On 2026-10-16 the startup/WRAM/FF74/OAM-DMA fixes in `f38d070` were compared
with pre-fix commit `4039af7` using the same verified v7.0 bundle. Both runners
were built headlessly in Release mode; existing expected results and suite gates
were retained. The results are observations from this comparison, separate from
the registered baseline counts above.

| Coverage | Before | After | Change |
| --- | --- | --- | --- |
| Required conformance | 168 pass, 0 fail | 168 pass, 0 fail | No regressions |
| AGE research | 9 pass, 75 fail | 9 pass, 75 fail | All 84 case statuses identical |
| SameSuite research | 36 pass, 44 fail, 2 informational | 36 pass, 44 fail, 2 informational | All 82 case statuses identical |
| AGE visual captures | 17 captures | 17 captures | Every PPM byte-identical |

This covers **334 paired report entries and 664 ROM executions**; the two
informational rows on each side do not execute ROMs. Existing research failures
are still failures; unchanged results do not qualify them as correct hardware
behavior. The independently verified BullyGB results remain five aggregate passes
and the CGB0 DIV disagreement documented in the build guide.

[Machine-readable provenance](../tests/fixtures/external-reference/bullygb-regression.json)
records full commit IDs, bundle and runner hashes, build settings, case-status
fingerprint and comparison limits. Source snapshots, upstream ROMs, generated
captures and raw logs stayed outside the repository. The two after-runner hashes
distinguish the existing headless build used for research captures from the fresh
build used for conformance; both use the same committed core sources. Elapsed
execution times are not used as performance evidence.

Reproduce each side by checking out the recorded commit into an isolated
directory, verifying/extracting the pinned bundle, and configuring:

```sh
cmake -S /external/source -B /external/build -DCMAKE_BUILD_TYPE=Release \
  -DGAMEBOY_BUILD_TESTS=ON -DGAMEBOY_BUILD_SDL=OFF \
  -DGAMEBOY_TEST_ROM_DIR=/external/game-boy-test-roms-v7.0
cmake --build /external/build --target gameboy_test_runner --parallel 2
ctest --test-dir /external/build -L conformance -j4 --output-on-failure
python3 /external/source/tests/run_external_suites.py \
  --runner /external/build/gbb_test_runner \
  --rom-root /external/game-boy-test-roms-v7.0 \
  --suite age-test-roms --suite same-suite --jobs 4 \
  --output /external/results/report.md
```

Use distinct build and result directories for the two commits. Compare case IDs
and profiles as well as totals; comparing totals alone can hide a new failure
balanced by an unrelated improvement.

The follow-up APU model-filter correction now uses one resolver for both the
metadata report and direct SameSuite CTest registration. Explicit suffixes map
only to their named hardware: CGB0, CGB-C, CGB-E and AGB. CGB-A/B/D have no
implemented profile and remain visible as unsupported entries, including the
unsupported members of a mixed revision label. Unknown explicit suffixes are
also unsupported. Untagged digital-APU tests retain the CGB-E research default;
the two pre-CGB DIV-trigger tests retain DMG.

On the same pinned bundle and unchanged emulator core, the corrected report is
**36 pass, 40 fail, six unsupported and two informational**. All 76 shared case
IDs/profiles retain their previous status. Six wrongly selected model rows are
removed, two correctly selected rows are added, and six unsupported revision
rows become visible. Thus the lower failure count reflects applicability, not
an APU accuracy improvement. Direct CTest registration agrees on all 70 runnable
APU profiles and six unsupported rows; unsupported direct tests are marked
skipped explicitly. Remaining applicable APU failures still fail their diagnostic
tests, and research release-gate policy is unchanged.

### Additional pinned-bundle suites

The v7.0 archive is also treated as a source of matrix cases rather than only a
collection of hand-maintained paths. The matrix runner discovers every
machine-readable `.gb` in the supported suite directories below, so a new ROM
cannot be silently omitted from the opt-in matrix. Upstream helper/manual
directories are intentionally excluded, as are GBMicrotest power-on fixtures
from this post-boot matrix; those images do not implement
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

AGE and SameSuite are covered by the opt-in metadata-driven external-suite
runner rather than the per-model matrix. AGE cases with reference PNGs are
captured at the runner's `LD B,B` completion point and compared with the
upstream image, including the documented non-CGB compatibility palette. AGE
machine cases use the Mooneye-compatible Fibonacci result registers. SameSuite
APU cases select DMG, CGB0, CGB-C, CGB-E or AGB from the upstream filename
and documentation, with unsupported revisions retained explicitly; DMA, interrupt, and PPU cases use the broad CGB-E research
profile until revision coverage is documented. SGB probes are retained as
informational cases because they expose commands rather than the Fibonacci
result ABI. Research failures remain visible in `external-suite-report.md` but
are not release regressions until a case is promoted to the `required` gate.

Each external report now includes stable failure clusters such as
`ppu/mode3-background`, `ppu/stat`, `cgb/speed-switch`, and
`apu/channel_1`. These labels are derived from the upstream case path and are
triage aids, not claims that every case in a cluster has the same root cause.

The test runner also supports `--trace-io PATH`. It records CPU-cycle, PPU
line/dot, mode, address, and value for I/O writes, allowing a visual or PCM
failure to be reduced to the exact register-write boundary. The generic
`scripts/differential_test.py` harness can invoke an adapter around a trusted
reference emulator with repeated `--reference-arg` options. Adapter arguments
may use `{rom}`, `{model}`, `{max_cycles}`, `{frame}`, and `{trace}`; arguments
are passed directly without a shell. For example:

```sh
python3 scripts/differential_test.py --candidate build-desktop/gbb_test_runner \
  --reference /path/to/reference-adapter --reference-arg={rom} \
  --reference-arg=--model --reference-arg={model} --rom tests/fixture.gb \
  --output /tmp/differential --frame
```

This makes the reference connection reproducible while keeping the external
emulator and any proprietary ROM/reference assets outside the repository.

BullyGB v1.2 now has an explicit terminal-screen protocol and a separately pinned
research run; see [build and testing](build-and-testing.md#bullygb-external-research-suite).
The remaining visual or interactive suites include cgb-acid-hell, MBC3 Tester, rtc3test, TurtleTests, and parts of
little-things-gb. Their ROMs and references remain documented by the bundle,
but they are not registered by the external runner yet: each needs
suite-specific frame timing, input scripting, or screenshot selection. This
distinction prevents a timeout or an unreviewed screenshot from being reported
as a core emulation failure.

The `gameboy_hardware_model_matrix_contract` test complements the ROM suites by
constructing each supported profile directly. It checks the post-boot CPU
registers, DIV handoff value, serial-divider phase, JOYP selection, APU channel
startup state, CGB register defaults, and save-state round trips. Automatic
selection is also checked for ordinary DMG, SGB-capable, and CGB-capable
cartridges. This is a digital profile contract; it does not claim to model
analog clock tolerance, LCD response, DAC variation, or full Game Boy
Advance/Game Boy Player hardware. Separate boot contracts cover the AGB
startup profiles; they are not added to the eight-model silicon matrix.

Native CGB post-boot startup now gives cartridges `DE=FF56, HL=000D`;
monochrome compatibility retains `DE=0008, HL=007C`. The fast startup path
previously used the compatibility values in both modes. The independently
checked original boot handoff agrees on CGB0, CGB-C and CGB-E for header flags
`00`, `80` and `C0`, with two takes per case. The authored cartridge contract
also observes these registers through WRAM writes after startup, reset and
save-state restoration. Evidence is recorded in
[`cgb-boot-registers.json`](../tests/fixtures/external-reference/cgb-boot-registers.json).

To reproduce the CPU-register comparison, build `scripts/sameboy_cgb_capture.c`
against the pinned SameBoy Core, then run:

```sh
python3 scripts/cgb_boot_register_probe.py \
  --runner build/gbb_test_runner --sameboy-capture /path/to/sameboy-capture \
  --boot-rom /private/cgbE_boot.bin --model cgb-e \
  --logo-source /private/cartridge.gbc --output /private/boot-register-review
```

The optional logo source supplies only the header field needed by the original
boot's validation. Keep that input and the generated cartridges outside the
repository. Use each revision's corresponding boot image for `cgb0` and `cgb-c`.
The reference command `--boot-registers` stops at boot unmap with `PC=0100`,
before cartridge execution; failure to reach it is an error. The GBB witness
must be its initial PC event and must complete the authored result protocol.
This checks CPU registers only, not peripheral startup phases or screenshot
accuracy. The outstanding release screenshot differences remain unqualified.

The revision matrix is intentionally explicit: `dmg0`, `dmg` (DMG-B), `mgb`,
`sgb`, `sgb2`, `cgb0`, `cgb-c`, and `cgb-e` are selectable in the test runner. The historical
`cgb` spelling remains an alias for `cgb-e` so existing scripts and save states
keep their behavior. The CGB-C profile uses the early-revision APU and DIV
phase rules, while CGB-E enables the late-revision envelope (NRx2 zombie-mode)
behavior. The focused APU contract covers the reviewed CGB-C/CGB-E envelope
write mismatch; channel alignment, PCM register visibility, and noise LFSR
startup remain covered by shared contracts until a reproducible revision-only
mismatch is measured.

The Wilbert `acceptance/timer/timer_if.gb` fixture now passes across the exposed
post-boot profiles. Its normal timer-interrupt vector boundary is covered by the
same 20-cycle CPU dispatch contract and by the complete timer unit suite; the
matrix no longer carries a known-failure exception for this fixture. In
monochrome compatibility mode on CGB hardware, the timer is held for one
internal dispatch cycle, while the existing native-CGB software-request path
retains all twenty timer clocks.
Both modes have explicit software-request contracts; the compatibility fixture
agrees with the pinned SameBoy reference on CGB0, CGB-C and CGB-E, recorded in
`tests/fixtures/external-reference/cgb-compatibility-timer-if.json`. A
header-modified native-CGB variant still differs from the reference and needs
separate overflow-edge investigation; this correction does not change that path.

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
When SGB/SGB2 is selected explicitly, that profile also overrides a cartridge's
CGB compatibility flag so dual-mode software sees the DMG-compatible SGB bus
and can issue its border-transfer commands.
The SGB path is a deterministic clean-room adapter: JOYP command packets are
decoded at the bit level by `gameboy::SgbAdapter`, and
`PAL01`/`PAL23`/`PAL03`/`PAL12` set RGB555 palettes,
and `ATTR_BLK`/`ATTR_LIN`/`ATTR_DIV`/`ATTR_CHR` update the 20×18 tile attribute
map used by the Game Boy viewport. The transfer commands `PAL_TRN`/`PAL_SET`
and `ATTR_TRN`/`ATTR_SET` retain and apply the SGB palette and attribute-file
memories, while the `_TRN` commands sample the live indexed Game Boy image and
encode it into SNES-side transfer latches. The core now exposes a deterministic 256×224 SGB
border framebuffer: it decodes the SNES 4bpp tile data, tilemap flips, and
RGB555 border palettes. Transparent border pixels expose the live Game Boy
viewport; opaque border pixels may cover it. Before a cartridge uploads a custom border, the
core renders a deterministic BIOS-style fallback frame so SGB output does not
collapse to a black letterbox. The command receiver is armed at adapter startup,
matching the first direct `00` start pulse used by licensed software; this is
required for the initial multiplayer probe and subsequent border transfers.
`MASK_EN` implements disabled, freeze, black, and
color-zero viewport modes. `MLT_REQ` supports deterministic one-, two-, and
four-player polling IDs with independent button states. SDL maps keyboard and
the primary gamepad to player 1, and additional gamepads to players 2-4; a
second desktop keyboard layout controls player 2 when SGB multiplayer is
active. These behaviors are covered by core tests, adapter diagnostics, and
save states (multiplayer fields were introduced in version 37; the ordinary
GB container is currently version 42). The NTSC SGB1 profile uses its approximately
4.295 MHz Game Boy clock for both frontend pacing and 48 kHz audio resampling;
SGB2 stays at 4.194304 MHz
([Pan Docs SGB clock](https://gbdev.io/pandocs/SGB_Functions.html)).
PAL-host SGB1 clock selection is not modeled.
SGB VRAM
transfer commands retain their payload for the hardware-like five-frame
transfer latency before the new data becomes visible. This is deliberately not
a full SNES emulation path: the real adapter relies on SNES-
side execution, graphics, and audio ([Pan Docs SGB overview](https://gbdev.io/pandocs/SGB_Functions.html)), so complete SGB compatibility would
require either those SNES subsystems or an equivalent dedicated host model.
The frontend-neutral SDL and web presentation paths now consume the SGB frame
dimensions from the core descriptor, so desktop, Android, and browser builds
can display the border without a frontend-specific decoder. HLE does not
execute SNES-side sound commands. Experimental desktop firmware playback
adds combined GB/SNES audio using user-owned program images and a bundled
original SPC IPL (with an external-image override);
Android and Web still use HLE. Bundled SGB/SGB2 replacement boots provide
the GB-side header bootstrap, not a replacement for those SNES-side images.
Full SNES graphics/menu rendering, the four deferred title-reference frames,
and independently validated host/adapter timing remain limitations. The
clocked LCD bridge improves transfer availability but its producer/read
collision behavior is not hardware-qualified. Current host headroom is also
unqualified: the latest serial measurement failed the unchanged gate. See
[SGB boot/bridge evidence](sgb-boot-validation.md#clocked-lcd-bridge) and the
[firmware playback guide](sgb-host.md#experimental-desktop-playback).
These limitations do not imply failures of ordinary DMG or CGB emulation.
The opt-in [SGB command inventory](sgb-validation.md) identifies unimplemented
SNES-side commands in traces without claiming to emulate their effects.
The spec-derived SGB reference checks and performance procedures are recorded
in [SGB validation](sgb-validation.md); they are not a substitute for
hardware-captured reference frames.

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

Saves now capture live SB/SC rather than their backing I/O cache, so saving
immediately after an external peer edge (before the receiver's bus tick)
preserves the actual partial byte. No payload/version change is required.
[DMG replacement serial validation](dmg-boot-serial-validation.md) covers its
first-transfer phase, mixed boot/default links, interrupt timing and restoration;
it does not establish physical reset equivalence or title-level trading coverage.

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
reviewed `*.txt` files are present in that directory. Six reviewed normalized
SameBoy references are checked in and therefore enable that CI gate; the
deterministic software fixtures remain a separate baseline. SameBoy is a
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
for its two pre-CGB DIV-trigger ROMs, exact supported revisions for explicit
CGB tags, AGB for `-A`, and CGB-E for untagged digital APU tests. Unsupported
CGB-A/B/D tags are visible as skipped direct tests or unsupported report rows. This suite is diagnostic until its
revision-specific failures are resolved; it is not included in the release
gate by default.

The opt-in `scripts/samesuite_alignment_reference.py` narrows the CH1/CH2
alignment failures using four individually hashed ROMs from pinned bundle v7.0
and SameBoy 1.0.3 (`208ba4afabffab9edde416f2dbb8ae459e34adb8`). It runs the
unmodified tests on CGB-E with the same owned CGB boot image in both cores.
Each test switches to double CPU speed. Both explicit result protocols and
complete 48-byte WRAM curves are collected twice; a separate frame-16 capture
must follow the protocol completion clock. GBB's frozen completion-register
signature is also checked. CPU T-cycles and SameBoy's 8-MHz clocks remain in
separate fields and are compared only within their own emulator.

```sh
python3 scripts/samesuite_alignment_reference.py \
  --rom-root /external/game-boy-test-roms-v7.0/same-suite \
  --runner ./build/gbb_test_runner \
  --sameboy-capture /external/sameboy-cgb-capture \
  --output /external/samesuite-alignment-results
```

Build the reference adapter as documented in
[the GBVisualizer reference guide](gbvisualizer-reference.md); the pin descriptor
is `tests/fixtures/external-reference/samesuite-alignment.json`. ROM hashes are
checked before execution, outputs require a fresh directory, and no external
source/ROM/assets are bundled. Reports retain commands, binary/boot/ROM hashes,
protocol clocks, repeated curves and differing sample indices. The reference
executable must be built against the pinned SameBoy Core revision; its binary
hash is recorded, while the script does not infer a library revision from that
hash.

Before the 2026-10-16 clock correction all four tests passed in SameBoy and failed in GBB, with repeatable
protocols and curves. Each curve has three 16-sample groups, using 0/1/3 NOPs to
vary either channel-trigger placement (`align`) or NR52 enable placement
(`align_cpu`). Within a group, the delay before the PCM read ranges from 0 to
15 NOPs. CH1 amplitude is 08 and CH2 amplitude is 80 in PCM12. The first nonzero
sample positions, numbered from zero, were:

| Test group variable | GBB, both channels | SameBoy, both channels |
| --- | --- | --- |
| Channel-trigger phase | 11 / 11 / 11 | 13 / 12 / 12 |
| NR52 enable phase | 11 / 11 / 11 | 13 / 13 / 13 |

CH1 and CH2 agree exactly after normalizing the high nibble. For trigger-phase
tests the differing indices are 11, 12, 27 and 43; for enable-phase tests they
are 11, 12, 27, 28, 43 and 44. The variable shift points to the APU-enable-relative
clock phase as a useful next hypothesis; a uniform trigger-delay adjustment
would not express all these observations. These are register-amplitude timing
measurements, separate from analog waveforms and from the SGB SPC700 driver.

The full comparison returns **1** with `qualification=false` unless
both conformance protocols pass and both curves match. Contract tests cover pins,
PCM fields, complete pass/fail signatures, fail-closed reports and hash rejection
before execution. Optional native tests require `GBB_TEST_RUNNER`,
`GBB_SAMEBOY_CGB_CAPTURE` and `GBB_SAMESUITE_ROM_DIR`; they check repeated frozen
curves, cross-channel symmetry and successful comparisons. CTest registers
`gameboy_samesuite_alignment_reference_contract`.

The independently authored `scripts/cgb_pulse_phase_probe.py` now varies NR52
enable and trigger phases separately over 0..3 NOPs, at both CPU speeds and for
both pulse channels. Each of the 64 cases restarts NR52/DIV for each of 32 PCM
observations. It uses frequency 2047, duty 50%, volume 8 and straight-line code;
timer, interrupts and LCD remain disabled until the bounded log is complete.
PCM reads finish 12 + 4*n CPU T-cycles after the trigger, and NR52 status is
recorded separately. Guard/completion checks and active-channel status reject
incomplete logs; two captures per core check repeatability. No upstream program
or expected-results table is used to build the controls.

Before correction, all 32 normal-speed curves matched; all 32 double-speed
curves differed. At normal speed the first nonzero observation was index 4 in
both cores. At double speed it was index 10 in GBB, compared with 11 for even
trigger padding and 12 for odd trigger padding in SameBoy. Moving enable timing
relative to DIV did not affect these curves. Both channels matched after nibble
normalization. This independently confirmed the enable-relative clock phase.

CGB-E/AGB inactive pulse triggers now round the initial countdown to the
1-MHz clock established by NR52 enable, replacing the fixed inactive-channel
phase adjustment on that path. The clock advances even with channels disabled
or presentation audio muted. Active retrigger alignment is covered by the
follow-up correction below; other revision paths retain their existing
behavior. All 64 authored cases now match, as do the complete
48-byte curves and explicit pass protocols of the four pinned SameSuite tests.
The full SameSuite report improves from 36 pass / 40 fail to 55 pass / 21 fail;
six unsupported revisions and two informational entries remain unchanged.
All 19 status changes are failures becoming passes, including one AGB frequency
change test. These are reference comparisons, not physical hardware validation
or qualification of the SGB SPC700 sound driver.
The measured curves, changed case IDs, source/executable hashes and successful
168-case conformance result are recorded in
`tests/fixtures/external-reference/cgb-pulse-phase.json`.

Save states retain the two-bit clock phase in previously unused bits 3-4 of the
APU sequencer byte, preserving the existing layout. Old states restore phase
zero because they did not record it. Regression contracts restore before a
trigger at all four phases for both channels, and verify subsequent PCM edges;
direct core contracts also cover muted output and batched disabled-channel
clocking. CTest registers `gameboy_cgb_pulse_phase_probe_contract` for the
offline tools, with an optional complete paired native matrix.

The follow-up `scripts/samesuite_frequency_reference.py` pins the unmodified
`channel_1_freq_change_timing-cgbDE.gb` from the same v7.0 bundle. It compares
all 16 packed immediate/late PCM observations and repeated explicit pass/fail
protocols against SameBoy 1.0.3, using CGB-E and the same owned boot. Eight
observations run at normal speed and eight at double speed; CGB-D is not
substituted or claimed. The shared capture code checks a frozen completion
signature and captures after protocol completion. Before correction only
observation 10 differed: GBB recorded `0f` and SameBoy `00`. Normal-speed
observations already matched.

`scripts/cgb_pulse_frequency_probe.py` independently isolates the boundary with
both pulse channels, frequency-high writes, frequency-low writes and an
unchanged-frequency high-register control, at both CPU speeds. Each of the 12
cases sweeps 64 write delays, starting from frequency 2044, duty 12.5% and
volume 15. PCM12 is read 12 and 96 CPU T-cycles after each write; NR52 is checked
separately. Timer, interrupts and LCD stay disabled during measurement. Before
correction, only the two double-speed high-register cases differed, at delay
indices 48 and 56; both low-register and unchanged-frequency controls matched.
All observations repeated and both channels agreed after nibble normalization.

CGB-E frequency-high writes now retain the reload collision through the
current NR52-relative 1-MHz interval, with the countdown still anchored to the
original clock edge. Frequency-low writes retain the single-clock collision;
AGB and the other hardware profiles keep their existing path. APU clock phase
now advances at each cycle in the detailed path and by the full span in the
batched path, so window closing is independent of tick granularity and muted
audio. The existing serialized reload flag and clock phase preserve this
window across save/load without a wire-format change.

All 12 authored cases and the pinned frequency observations/protocols now match.
The full SameSuite report improves from 55 pass / 21 fail to 56 pass / 20 fail,
with no previous pass lost; six unsupported revisions and two informational
entries remain unchanged. In particular, the separate AGB frequency-write test
still passes. Direct contracts bracket the window at offsets 0..4 for both
channels, both registers and CGB-E/AGB, using single-cycle and batched ticks.
Save/load contracts restore inside and immediately outside the window.
These remain reference comparisons, separate from physical hardware
qualification and SGB SPC700 audio. The pin is
`tests/fixtures/external-reference/samesuite-frequency.json`; measured before/
after evidence is `tests/fixtures/external-reference/cgb-pulse-frequency.json`.

The opt-in `--lifecycle` frequency matrix now checks 48 additional cases on
the unchanged `b31fff5` core: both channels, both initial CPU speeds and
high/low/unchanged-frequency/no-write operations, following an active retrigger,
a CPU-speed switch, or a switch then an active retrigger. Every case sweeps 64
delays. The no-write operation replaces LDH with three NOPs of equal CPU cost;
it does not touch either frequency register. Initial and per-observation KEY1
witnesses verify actual speed and a cleared prepare bit, independently of the
requested ROM variant. Four-byte PCM12/PCM12/NR52/KEY1 records occupy exactly
C100..C1FF, followed by the C200 guard. Completed logs are frozen before the
LCD is enabled for capture. NR52/DIV are reset for each observation, and switch
cases restore the initial CPU speed before the next reset.

All 192 executions repeated with valid records and requested speed changes.
Six of 48 cases matched SameBoy; 42 differed. At normal speed, retrigger-only
low-register, unchanged-frequency and no-write cases matched for both channels;
the high-register cases differed. All double-speed retrigger-only cases and all
switch cases differed. CH1 and CH2 were identical after nibble normalization.
Even the double-speed retrigger-only no-write control differed at indices 28,
36, 49 and 57: first nonzero immediate/late observations occurred at 49/28 in
GBB and 50/29 in SameBoy. The original 12 cases still match, and their ROMs
remain byte-identical to the recorded pin hashes.

These controls show that frequency-write latch timing alone cannot explain
the lifecycle discrepancies. STOP-transition clocks and retrigger phase need
separate isolation before further corrections. No core timing is changed by
this matrix, no hardware baseline is qualified, and mismatches still return
exit 1. Per-case hashes, differing indices and first differing observations
are recorded in `tests/fixtures/external-reference/cgb-pulse-frequency-lifecycle.json`.
The subsequent `scripts/cgb_pulse_retrigger_probe.py` isolates active restart
timing with equal-cost no-retrigger controls and no frequency writes or STOP
after the measured operation. It covers both channels and CPU speeds, varying
the initial trigger-to-operation gap over 0/1/2/3/8/32 NOPs. Each of 48 cases
samples 64 independent read delays, with constant frequency 2044, duty 12.5%
and volume 15. PCM reads finish 12 + 4*n CPU T-cycles after the second trigger
or equal-cost control; NR52 and KEY1 are recorded separately. Short gaps vary
the trigger's clock phase, while longer gaps also check retained duty position.

Before correction 36/48 isolated cases matched: all normal-speed cases and
all no-retrigger controls. All 12 double-speed active-retrigger cases differed.
For either channel, first nonzero sample indices were:

| Gap in NOPs | GBB before | SameBoy / GBB after |
| --- | --- | --- |
| 0 | 54 | 56 |
| 1 | 54 | 55 |
| 2 | 54 | 56 |
| 3 | 54 | 55 |
| 8 | 46 | 48 |
| 32 | 22 | 24 |

CGB-E active retriggers now use one 1-MHz startup tick followed by alignment
to the NR52-relative clock, retaining duty position. The inactive path still
uses two ticks. AGB retains its previous active delay and frequency-write path;
other revision paths are unchanged. All 48 isolated curves now match and repeat.
The lifecycle matrix improves from 6/48 to 16/48 matches with no previously
matching case lost: every retrigger-only case now matches, and all 32 remaining
differences involve a speed switch. This does not resolve STOP-transition
clocking or qualify physical hardware behavior.

The full SameSuite run retains all 84 statuses: 56 pass, 20 fail, six unsupported
and two informational. All 168 conformance tests still pass. Direct contracts
check both channels, CGB-E/AGB, muted output and single-cycle/batched warm-up;
save/load restores before a retrigger with varied clock and duty phases without
a format change. Before/after hashes, sample differences and case counts are
recorded in `tests/fixtures/external-reference/cgb-pulse-retrigger.json`.
The subsequent `scripts/cgb_stop_apu_probe.py` adds active, configured-but-not-
triggered (`armed`) and powered-off (`off`) APU controls around STOP. Armed
channels are triggered afterward; off controls enable/configure/trigger the
APU afterward. Single-NOP controls keep the initial speed and do not perform
STOP's DIV reset. NOP matches this core's nominal four-T-cycle STOP instruction
path, without assuming equivalence of transition time or clock effects. Each exact
program is compared between cores, at both initial CPU speeds, for both channels and pulse
periods 16 and 28, retaining 12.5% duty and volume 15. The non-power-of-two
period helps distinguish pulse-phase offsets hidden by the period-16 control.

All 48 cases collect 128 independent PCM12/NR52/KEY1/DIV/TIMA/IF tuples, with
TIMA seeded to 0x30 and TAC=0x04 (the slow timer). DIV/TIMA/IF reads finish 60/80/100
CPU T-cycles after PCM. No interrupt is serviced; the LCD remains off until
the complete C100..C3FF log is frozen, with its adjacent C400 guard intact.
KEY1 witnesses confirm initial/final speeds and a cleared prepare bit. Switch
cases restore initial speed before resetting the next experiment. Reports
separate PCM agreement from DIV/TIMA/IF agreement, masking unused IF high bits.

All 192 captures repeat with valid records and speed witnesses:

| Operation / APU during operation | Cases | PCM matches | Timer matches | Complete matches |
| --- | --- | --- | --- | --- |
| NOP, all APU states | 24 | 24 | 24 | 24 |
| Switch, active | 8 | 0 | 0 | 0 |
| Switch, armed | 8 | 8 | 0 | 0 |
| Switch, powered off | 8 | 8 | 0 | 0 |

DIV and low IF trajectories agree in all 48 cases. In every switch case,
all TIMA observations are 0x30 in GBB and 0xB0 in SameBoy, independently of APU
state. PCM agrees when the channel starts after the switch. The active-channel
PCM mismatch is identical across channels; circular alignment on the sample
grid gives these equivalent shifts of SameBoy relative to GBB:

| Transition | Pulse period | Sample shift | Equivalent APU clocks modulo full pulse cycle |
| --- | --- | --- | --- |
| Normal to double | 16 | 4 | 8 modulo 128 |
| Normal to double | 28 | 68 | 136 modulo 224 |
| Double to normal | 16 | 1 | 4 modulo 128 |
| Double to normal | 28 | 9 | 36 modulo 224 |

These are phase equivalences at the post-switch sample resolution, not absolute
transition durations. The witnesses separate timer-transition accounting from
already-active channel phase, but do not establish the exact STOP pause or
which clocks run during it. No CPU/APU timing changes are made in this step.
The core remains `226742c`; evidence and per-case hashes are in
`tests/fixtures/external-reference/cgb-stop-apu.json`. Existing harness budgets
remain unchanged; these longer controls explicitly request larger bounded
budgets. The subsequent instruction-window measurements below resolve the
reference intervals before changing the CPU/APU model.

`scripts/cgb_stop_timing_probe.py` first measured a single STOP/NOP operation and
resumed instruction on the unchanged `9b579d8` core. This is the pre-transition
baseline. The optional reference
adapter's `--pc-clock-window` mode uses public execution callbacks and accumulated
`GB_run` 8-MHz clocks. It records at most eight callback entries, plus KEY1,
DIV, TIMA, PCM12 and BC witnesses. Timestamps identify the API call containing
the callback, not a physical opcode-fetch edge. GBB uses its existing first-PC
witnesses at operation entry and after the first resumed instruction, with
explicit Mooneye completion. Thus an early PC advance alone cannot qualify
as completed resumption. CPU T-cycles and reference 8-MHz clocks stay separate;
the GBB comparison conversion explicitly assumes its current atomic STOP.

All 20 cases repeat, and all register-effect comparisons agree. The ten NOP
controls match in time; all ten switch cases differ. For a first resumed NOP
(or equal-cost INC B), the reference intervals are:

| Transition | Entry to first resume callback | Entry through first resumed instruction | First resume call gap | Later NOP call gap |
| --- | --- | --- | --- | --- |
| Normal to double | 131,088 | 131,096 | 8 | 4 |
| Double to normal | 262,150 | 262,166 | 16 | 8 |

Every value in this table is in reference 8-MHz ticks. Results are identical
with active, armed and powered-off APU states. GBB accounts for only eight
CPU T-cycles across operation plus resumed NOP, equivalent to 12 8-MHz ticks
under its current two-instruction speed partition. NOP-only controls calibrate
to 16 ticks at normal speed and eight at double speed.

The resumed INC B changes B from 0x10 to 0x11 exactly once in both cores, with one
callback per instruction. The two-cycle resumed `LD B,0x11` also agrees in register
effect; its first-resume call gaps are 12/24 ticks for normal-to-double/double-to-
normal, versus nominal 8/16. Together these controls locate a gap equivalent
to one additional machine cycle on the first resumed instruction, rather than
an instruction replay or a permanent new-speed error. Later NOP cadence is
nominal. TIMA still changes from 0x30 to 0xB0 across switches in the reference.

The intervals are consistent with a long new-speed clock interval plus
direction-dependent boundary overhead, but these API observations do not by
themselves prescribe hardware clock gating. No CPU/APU timing or save-state
layout changes are made in this step. Per-case intervals, callback records,
ROM/executable/library hashes and limits are recorded in
`tests/fixtures/external-reference/cgb-stop-timing.json`.

The CGB-E profile (including the `cgb` alias) now stages that transition. After
the old-speed STOP fetch, 131,072 new-speed CPU clocks advance the timer and
peripherals without fetching instructions. A normal-to-double boundary adds
12 new-speed clocks with DIV held; the first resumed instruction adds four
clocks with DIV held in either direction. The boundary model is constrained
by both pulse periods, active/armed/off APU controls and the DIV/TIMA witnesses;
it is an independently authored reference model, not hardware qualification.
The first resumed instruction executes once and subsequent cadence is nominal.
Other hardware revisions retain their existing transition behavior.

Each settling step consumes four CPU clocks, allowing frames and captures to
proceed and interrupt requests to accumulate until resumption. Save-state
version 44 appends the countdown, resumption flag and normalized bus clock;
checkpoints cover the long pause, short boundary and first resumed instruction.
Invalid countdowns reject transactionally. Versions 1-43 remain loadable,
without inventing a transition for their saved atomic STOP.

All 48 STOP/APU cases now match PCM and DIV/TIMA/IF observations, compared
with 24 full matches in the atomic baseline. All 48 frequency lifecycle cases
also match, up from 16 retrigger-only matches; both matrices repeat and retain
channel-pair agreement. All 84 SameSuite statuses are unchanged, and all 168
conformance ROMs pass. The authored ROMs and independently built reference
captures are summarized with hashes in
`tests/fixtures/external-reference/cgb-stop-transition.json`.

The timing probe's version-2 report uses a direct bus clock accumulated at the
actual CPU speed. NOP controls and all 20 register effects still match.
The switch windows are 131,100/262,164 8-MHz ticks through a resumed NOP,
compared with reference API-call boundaries at 131,096/262,166. Those residual
+4/-2-tick differences remain explicit; callback boundaries are not hardware
fetch timestamps. Next resolve that boundary anchoring and validate LCD-on
PPU/bus behavior before extending the staged model to other revisions.
LCD-off PCM fixtures do not validate the LCD-on VRAM/OAM locking behavior
[described in Pan Docs](https://github.com/gbdev/pandocs/blob/master/src/CGB_Registers.md#ff4d--key1spd-cgb-mode-only-prepare-speed-switch).

A private commercial-cartridge follow-up separates fast-startup state from
runtime timing. Super Mario Bros. Deluxe and Shantae both receive the native
DE/HL handoff above when booted through the original reference or the same
GBB replacement image. Correcting fast-startup DE/HL leaves their reviewed
600-interval images unchanged. With the same replacement boot in both cores,
STOP begins at matching elapsed bus clocks, but the first resumed instruction
still finishes four 8-MHz ticks later in GBB. The subsequent LCD/interrupt
schedule diverges further. Repeating the authored 20-case STOP matrix retains
ten matching NOP controls and twenty matching register effects, with the
existing switch-boundary differences intact. These observations locate a
remaining runtime question; they do not establish its hardware cause or
qualify the outstanding screenshots. Running the same boot image also does
not establish identical undefined RAM contents or every peripheral phase.
No fitted frame/time shift or CPU timing
adjustment is accepted as a consequence of the register correction.

The independent `cgb_lcd_startup_probe.py` now narrows that investigation with
24 authored cartridges. Each disables LCD/APU/timer/interrupts, clears its WRAM
log, selects normal or double speed, and resets DIV before enabling `LCDC=87`.
It observes the first two VBlanks or mode-2 conditions either by polling with
IE/IME disabled or by an interrupt handler. The shared logger records a BC work
counter, LY, STAT mode, IF low bits, DIV and KEY1 speed. Handler/logging time is
part of the workload; these are not physical edge timestamps. Three pre-enable
NOP delays vary the DIV phase without fitting an offset between cores.

The pinned reference comparison repeats every capture twice and checks the
frozen log at completed PPU frames 4 and 8:

| Observation | Normal speed | Double speed |
| --- | --- | --- |
| VBlank polling | 6/6 match | 6/6 match |
| VBlank IRQ | 6/6 match | 6/6 match |
| STAT mode-2 polling | 6/6 match | 6/6 match |
| STAT mode-2 IRQ | 0/6 match | 6/6 match |

All 48 comparisons repeat and satisfy the authored completion, LY/mode,
request/acknowledgement and speed constraints; all logs remain frozen. In the
normal-speed STAT IRQ cases, GBB records BC counters 17/25 while the reference
records 16/24, across all three delays and both checkpoints. Other recorded
fields agree. The tool therefore returns failure, with `qualification=false`;
its offline tooling tests do not certify reference parity. Evidence and source
fingerprints are in
[`cgb-lcd-startup.json`](../tests/fixtures/external-reference/cgb-lcd-startup.json).
Polling agreement at this sampling granularity does not prove identical
request-edge timing. Next distinguish a first mode-2 request-edge shift from
CPU interrupt-recognition timing with a finer request/dispatch phase sweep.
No CPU/PPU timing, commercial screenshot baseline or release gate changes are
made on the strength of this counter difference.

The finer `cgb_stat_request_phase_probe.py` uses thirteen post-enable CPU-NOP
delays for separate primary IF, STAT-mode and LY reads. The common delay is
432 PPU dots before the variable NOPs and the read instruction; every variable
NOP adds four CPU clocks (four dots normally, two at double speed). Later
fields in a sample are separate reads at later times, not a simultaneous PPU
snapshot. Four further controls assert software STAT IF with LCD and STAT
sources disabled and IE/IME already armed; the first handler records a
foreground BC counter and an explicit handler guard. They cannot complete
through the foreground path without actually servicing the interrupt.

Before correction, 82/86 comparisons matched the pinned reference. All primary
LY/mode samples and all eight software-request controls agreed, but four
double-speed records differed in IF. For the primary IF sweep, the reference
first exposed STAT at phase 7, while GBB exposed it at phase 8. The other
three differences were later IF reads within records. At normal speed both
primary IF sweeps first exposed it at phase 2.

This exposed a concrete accounting error, independent of STAT arbitration:
`MemoryBus::tick` rounded every double-speed batch down separately. An IF read
advances `3 + 1` CPU clocks, so those calls produced only one PPU/cartridge dot
instead of two. Peripheral conversion now carries the fractional dot using
the existing 8-MHz normalized bus clock. The APU retains its separate alignment
phase; CPU timer/serial clocks are not rescaled. No save-format change is
needed. CGB0, CGB-C, CGB-E and the alias have direct instruction/split-tick,
save/load, warm-reset and cold-initialization contracts. The added instruction
contract rejects the pre-fix implementation.

After correction all 86 comparisons repeat, satisfy the authored constraints
and match, including the phase-7 double-speed IF sample. Evidence is recorded
in [`cgb-stat-request-phase.json`](../tests/fixtures/external-reference/cgb-stat-request-phase.json).
The original 48-comparison LCD-startup probe still matches 42/48: its six
normal-speed STAT IRQ counter differences remain. Neither this software
reference agreement nor the matching software requests qualifies every
hardware-generated interrupt-recognition boundary. Next measure first-vector
entry across different foreground instruction alignments; do not compensate
the commercial screenshots with a frame offset.

DIV/APU edges are dispatched at their timer-cycle boundary rather than being
queued until the end of a bus batch. The APU also models the hardware rule that
enabling it while the DIV/APU input is high skips the first falling-edge event;
the pending phase is retained in save states. This is covered by the core timer
tests and the corresponding SameSuite DIV-trigger ROMs. A suppressed first edge
is also treated as a non-length step when NRx4 enables a length counter: the
one-tick counter receives its extra clock and is reloaded by a simultaneous
trigger. This matches SameSuite's DMG `div_write_trigger_10` case; the ordinary
two-tick high-start case remains covered separately. The CGB-E/AGB inactive
square-channel path uses the NR52-relative alignment described above, including
the long-period `channel_[12]_volume_div` cases. CGB-E active restarts use the
clock alignment described above; other profiles retain their established timing.

Square duty writes now use a latched duty value: writes made while a channel is
active take effect at the next waveform boundary, while writes made while it is
inactive are applied for the next trigger. The latched and pending values are
stored in save-state version 19, and trigger suppression plus the
CPU-to-APU half-cycle phase are stored in version 20, so restoring during a duty
transition, period reload, or speed-switch half-cycle remains deterministic.
The CPU-to-APU phase is stable during normal-speed execution and advances only on alternating CGB
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
Starting HBlank DMA with the LCD disabled now performs exactly one immediate
block, matching the CGB register behavior; cancellation leaves the inactive
transfer status at `0x80` and does not copy a second block. HDMA source and
destination low nibbles remain masked by the register write path.

PPU VRAM arbitration follows the internal pixel-transfer boundary rather than
the one-dot-later CPU-visible STAT mode. This closes the boundary where a CPU
write could otherwise land after the fetcher had claimed VRAM but before STAT
reported mode 3. The contract suite now checks this separately from the
existing OAM handoff behavior.

The external runner keeps a full per-case log beside generated captures and
selects the actionable result line for the summary table. This makes timing
failures distinguishable from timeouts and preserves the register/video/audio
state printed by the runner for the next accuracy investigation.

For a focused timing investigation, the runner can emit one record after each
CPU step. `--trace-apu PATH` records the APU registers, divider, PCM12/PCM34,
and program counter; `--trace-ppu PATH` records LCDC/STAT, LY/LYC, scroll and
window positions, VRAM bank, IF, and the program counter. The same ROM can be
run through `scripts/differential_test.py` with two runner-compatible
executables to compare result status, traces, or `--frame` output and produce
a PPM difference image.

Machine-test failures also include a structured `RESULT` record with the
expected and observed Mooneye registers plus the first result-buffer bytes.
This is especially useful for AGE ROMs, whose detailed observations are left
in memory rather than sent over serial.

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


The first-vector follow-up (`scripts/cgb_stat_vector_probe.py`) records BC and
the interrupt's stacked return PC before terminating the first handler. Its
first vector instruction copies B to A; a jump to the terminal logger leaves
BC and the stacked address unchanged. The logger then pops that address into
DE. DIV, LY, STAT, IF and KEY1 are read sequentially afterwards: they are not
simultaneous edge timestamps. Completion is reachable only through the handler.

The public evidence in
`tests/fixtures/external-reference/cgb-stat-vector.json` covers 64 authored
cartridges: hardware mode-2 STAT or software IF with LCD/STAT sources off,
`INC BC; JR` or the original `INC BC; LD A,D; OR A; JR NZ` foreground,
eight post-trigger NOP phases, and both speeds. The same owned boot and
cartridges run in GBB and pinned SameBoy 1.0.3, with two takes per core at
completed PPU frame 4. All observations repeat and satisfy the authored
structural constraints. Exactly 48/64 comparisons match: all 32 software
controls and 16/32 hardware STAT cases. Nine normal-speed and seven
double-speed hardware cases differ, exclusively in BC and/or the stacked
return PC. In particular, the original normal-speed phase-zero loop preserves
the earlier BC=17 versus 16 distinction, with return PCs 0x1005 versus 0x1004.
The earlier matching double-speed loop therefore did not establish parity
across foreground alignments.

These architectural witnesses narrow the remaining investigation to hardware
STAT request/recognition relative to CPU instruction boundaries. They do not
separate sub-instruction request timing from interrupt sampling, or establish
which implementation matches physical hardware. The next useful control is
an equal-cost foreground IF-read sweep around those same boundaries, followed
by comparison of the internal request edge and interrupt sampling against the
reference. No CPU/PPU timing, commercial screenshot baseline or release gate
has been changed on this evidence alone. The research CLI retains exit 1 and
`qualification: false` for the measured mismatch.


The complementary `scripts/cgb_stat_boundary_read_probe.py` disables service
without changing setup duration: EI is replaced by a four-cycle NOP while IE=2
is retained and the initial DI remains effective. It unrolls the two foreground
patterns with taken JR +0 instructions, preserving nominal timing and register
and flag effects. Three loop counts around `floor(452 * speed_factor / loop_cost)`,
eight post-LCD NOP phases and every instruction slot define a common grid;
there are no per-core offsets or fits. The primary IF read starts at the end of
that prefix. Later observations record BC, STAT, LY, IF, DIV and KEY1; these
reads happen sequentially and cannot be interpreted as a simultaneous snapshot.

All 288 normal/double-speed comparisons match pinned SameBoy. Every case has
two repeatable captures per core at completed PPU frame 4, with 1,152 raw states
individually checked against the analyzer and source ROM hashes. At each speed,
19 primary reads report no STAT request and 125 report a pending STAT request;
the sweep therefore exercises both sides of the observable transition. Exact
BC counts validate prefix execution. Source fingerprints and all observations
are recorded in
[`cgb-stat-boundary-read.json`](../tests/fixtures/external-reference/cgb-stat-boundary-read.json).
The paired CLI exits 0 for this measured scope, retaining `qualification: false`.

This strengthens the CPU-visible IF-read controls but does not resolve the
16/64 first-vector differences. A CPU read consumes time and is sampled at a
bus phase; agreement can conceal a difference between physical request-edge
and read-sampling phases. The next diagnostic should anchor the accepted LCDC
write, first internal STAT request and interrupt dispatch on the same clock
axis, explicitly separating instruction callback boundaries from bus-access
phases. No CPU/PPU timing, private screenshot baseline or publication gate was
changed by this experiment; the unresolved release validation stays on hold.


The common-clock follow-up (`scripts/cgb_stat_clock_probe.py`) anchors both
cores immediately before the authored instruction at PC=0x1000. Every event
uses normalized 8-MHz clocks relative to that boundary; no offsets are fitted.
The existing GBB control-write observer timestamps accepted LCDC writes.
The separate pinned SameBoy internal-ABI adapter reads `cycles_since_run`
inside its LCD-status callback and adds the sum of preceding `GB_run` clocks.
Read/write/execution callbacks and instruction-step boundaries observe IF
without advancing time. An IF rise is reported as
`(last_clear_observation, first_pending_observation]`; callback observations do
not claim to locate the physical PPU edge within that interval.

The 64 earlier cases plus a pure-NOP IRQ calibration at each speed produce
66 comparisons and 396 bounded traces. Two observer-enabled runs per core
repeat. Final registers, the WRAM witness, normalized clock and serialized
state fingerprints agree with an observer-disabled run in every case.
SameBoy serializes an unused host-initialized RTC epoch for these ROM-only
cartridges. Only its eight timestamp bytes are masked in the copied serialized
buffer before fingerprinting; the emulator's timestamp and all other state
are untouched. An independent eight-run inspection found that this field alone
varied before masking. GBB fingerprints its entire serialized state.

The observed LCD-write phase difference is four CPU clocks at both speeds:

| Event, relative 8-MHz clocks | GBB normal | SameBoy normal | GBB double | SameBoy double |
| --- | ---: | ---: | ---: | ---: |
| Accepted LCD-enable write | 40 | 32 | 20 | 16 |
| NOP first-pending IF interval | (936, 944] | (928, 936] | (920, 924] | (916, 920] |
| NOP dispatch start | 944 | 936 | 924 | 920 |
| NOP dispatch end | 984 | 976 | 944 | 940 |
| NOP stacked return PC | 0x1075 | 0x1074 | 0x10E6 | 0x10E5 |

All 34 hardware cases show the same accepted-write separation. In the NOP
calibration, request intervals relative to the accepted LCD write agree:
(896, 904] normally and (900, 904] at double speed. Both cores begin service at
the first pending NOP boundary and take 20 CPU clocks for dispatch. In the
original normal-speed phase-zero loop, the reference is already pending at
relative clock 936, while GBB is still clear at 936. GBB executes another
`INC BC`, reaches BC=17 and dispatches at 952; SameBoy dispatches at 936 with
BC=16. Their intervals are disjoint on the shared instruction-origin axis.
This is direct evidence of a request-visibility separation preceding dispatch,
so a dispatch-only correction would not address the observed sequence.

The first-vector opcode callback is deliberately separate: GBB calls it after
the opcode fetch cycle, whereas SameBoy calls it before flushing that fetch's
pending cycle. In the NOP calibration those callbacks occur at 992/976 normally
and 948/940 at double speed. These numbers must not be compared as matching
physical-fetch phases or used to add another arbitrary timing compensation.

Exactly 48/66 architectural witnesses match: every software-request control
matches, the original 16 hardware mismatches persist, and both NOP return PCs
differ by one. The paired tool retains exit 1 and `qualification: false`.
Source hashes, per-case summaries and representative full timelines are in
[`cgb-stat-clock.json`](../tests/fixtures/external-reference/cgb-stat-clock.json).
The LCD accepted-write phase is now the concrete correction target, with bus
read-phase controls required alongside it: prior CPU-visible IF-read agreement
cannot independently certify asynchronous request timing. The next step is to
validate LCDC write timing across instruction forms and coordinate any fix with
IO read sampling and conformance tests, rather than shifting STAT edges alone.
No production CPU/PPU timing, private baseline or release gate has changed;
publication remains on hold.


The subsequent LCD bus-phase experiment tests four write addressing forms
against the same pinned Core, plus three IF-read forms padded before the read
to have the same 16-clock duration and final memory cycle. All eight write
cases expose a four-CPU-clock accepted-write separation; all 78 read cases and
52 cross-form relations already agree. The authored cartridges contain no
proprietary logo or artwork. Their source and before/candidate observations are
in [`cgb-lcd-bus-phase.json`](../tests/fixtures/external-reference/cgb-lcd-bus-phase.json).

A general native-CGB IO experiment sampled IO at the start of its final cycle,
paired read and write changes, and retained the existing interrupt stack-write
phase. It matched all eight writes, all 78 reads, all 66 first-vector cases and
all 48 LCD-startup comparisons. The 4,928 hardware-model cases had zero
regressions. However, the broader validation rejected it: one of 271 public
contracts failed because the DMA snapshot before RET was no longer at the
recorded boundary, and a previously exact private title comparison diverged
at all three capture checkpoints. No expected results were relaxed to make
these changes pass.

The private regression persists after applying the same diagnostic startup
reset to both cores (APU off, LCD restart, DIV reset, explicit JOYP/IE/IF). This
is a diagnostic change to initial conditions, not a replacement reference or
an approved baseline. Under that control the previous CPU gives three exact
images; the general IO and PPU-read variants differ by 52, 374 and 329 pixels.
A narrower LCD/IF-only variant restores those three exact images but matches
only 79/86 previously passing request/read-phase controls. Keeping the STAT
and LY read changes matches 86/86 but retains the private regression. Thus the
observed LCD-write separation alone does not establish a safe correction of
the coupled CPU/PPU interface. Early-write/read timing that is correct for the
first mode-2 witness is insufficient evidence for later STAT/LY polling.

The experimental IO timing changes were reverted. The general and two narrowed patches
are retained under `tests/fixtures/research/` solely to reproduce these rejected
experiments; they are never compiled by the build. The public fixture explicitly
sets `candidate_accepted: false` and records the rejection. Private screenshots,
ROMs, original boot firmware and private baseline hashes are not included.

The rejected candidate's full private run completed all 100 ROMs and 300
captures and differed from the approved baseline at 45 snapshots across 26
ROMs. Its initial reference image counts used the unsynchronized helper and
are superseded by the capture correction documented below; they do not
validate production HEAD. Original-boot handoffs also expose cartridge-dependent DIV values that
our fixed fast-start state does not reproduce, but the uniform-startup control
shows that startup differences alone cannot excuse the new runtime regression.
Ordinary HBlank/VBlank STAT and LY controls remain useful follow-up coverage,
retaining the existing first-line controls and private counterexample. The
release gate remains on hold and its baseline is unchanged.


A separate native interrupt-fetch correction resolves the architectural
mismatch without moving IO accesses or PPU edges. This core samples bus data
at the end of a fetch cycle. When a native-CGB request arrives during that
cycle, it now discards the prefetched opcode, restores its original PC for the
stacked return address, and consumes the fetch as the first four of the twenty
entry clocks. It does not execute that instruction or report it in executed
CPU history. Existing monochrome/compatibility recognition and interrupt
priority arbitration remain unchanged. No new persistent CPU state or save
version is needed. A host-only IRQ observer records the recognition point and
whether the fetch was already consumed; it does not alter emulation.

The final paired runs match 64/64 vector cases, 66/66 common-clock witnesses
including both NOP calibrations, 48/48 LCD-startup cases, 86/86 request-phase
controls and all 78 equal-cost read cases. The eight write-form architectural
witnesses also match, while the raw internal LCD-write phase separation remains
visible. Eighteen common-clock cases recognize the request after a discarded
fetch; all other cases retain their earlier recognition point. In the normal
phase-zero example, both dispatch intervals are 936..976 relative 8-MHz clocks
and both stack PC=0x1004 with BC=16. GBB observes the request during its fetch
at 944; the reference begins with it pending at 936. Those sample/callback
phases are explicit, not fitted offsets. Forcing all internal bus timestamps
to match is unnecessary for matching the architectural event.

Direct CGB0/CGB-C/CGB-E/alias tests cover STAT and timer requests during the
fetch at both native speeds, plus compatibility controls. They verify the
20-clock entry, single stacked original PC, untouched BC, excluded discarded
instruction, observer-disabled equivalence and save/load replay. The old
recognition algorithm fails all sixteen native boundary witnesses. CLI history
also excludes the discarded opcode. The hardware matrix retains 4,928 cases
with zero regressions; the full headless contracts validate the final build.
The general IO alternatives above remain rejected; the narrower IRQ correction
also preserves the private uniform-startup counterexample at three exact
snapshots. Historical before/candidate records remain in the fixtures.

The private elapsed-clock reference helper had a separate capture defect:
it wrote pixels before synchronizing SameBoy's deferred PPU work. Reading LY
only while subsequently printing metadata was too late. The public adapter
`scripts/sameboy_elapsed_capture.c` now flushes that work before writing pixels.
An independently authored ROM reproduces 47 changed pixels at one of eight
fixed clock targets, with unchanged elapsed clocks and legacy metadata and
two repeatable synchronized captures. Five malformed target lists fail closed.
This is a capture correction, not a time shift or change of palette. All 45
commercial reference targets were rerun twice with identical target/actual
clocks, PC, LY and LCDC to the unsynchronized run. The earlier unsynchronized
image-match counts are superseded.

With the retained fetch correction and synchronized reference, all 100 private
ROMs complete their 300 captures. The approved baseline still differs at 45
snapshots across 26 ROMs. Thirty now match the independent reference exactly;
fifteen remain unresolved across ten ROMs. These are private-input checks,
not publicly reproducible conformance results. An already-unresolved Robopon
capture changes from four to twelve differing pixels at the same reference
clock; it remains unqualified. No private baseline was updated and publication
remains on hold. Next isolate the remaining startup/peripheral-state and
framebuffer differences, retaining original-boot handoff evidence and the
synchronized capture anchor. Matching this focused IRQ scope does not qualify
those remaining commercial snapshots or the release as a whole.

The next controlled comparison uses the same independently authored boot in
both cores for the nine remaining CGB ROMs, at the fourteen unresolved clock
targets. Every target repeats twice in each core; PC, AF, BC, DE, HL, SP and
visible DIV/LCDC/LY agree at handoff. This does **not** establish identical
hidden peripheral or uninitialized memory state. Robopon's twelve-pixel
disagreement disappears under this startup control. The other thirteen captures
still differ, including the five-frame Conker and two-frame Bionic Commando
drift. Thus matching those handoff registers alone does not explain the
remaining differences. The separate SGB Donkey Kong case is excluded from this
native CGB comparison. These private checks neither replace the original-boot
results nor reduce the release's fifteen unqualified snapshots; its approved
baseline remains untouched.

`cgb-elapsed-anchor.json` records a public calibration using only the authored
frame-anchor ROMs and owned boot. Five fixed elapsed-clock samples, both CPU
speeds, optional LCD-enable delay and two takes per core produce eighty
captures. Fourteen of twenty paired images match exactly. All twenty agree on
LY and completed-frame count; none uses a fitted clock or frame offset.

Three differences retain first-frame presentation after LCD enable. For the
normal/no-delay case at 900,000 clocks, the bottom thirty rows differ (4,800
pixels); at 900,512 clocks, 4,755 pixels differ. The double/delayed case at
1,200,000 clocks differs at 15,365 pixels. Inspection of the pinned reference's
`GB_display_vblank` shows that it clears first-frame presentation output at
the first VBlank after enable. GBB keeps the preceding framebuffer contents.
This reference presentation behavior must be distinguished from the deferred
PPU synchronization defect already fixed in the adapter.

The other three differences each involve two adjacent pixels at an active
drawing boundary: normal/no-delay at 1,600,000 clocks (x=37..38, LY=111),
normal/delayed at 900,000 clocks (x=5..6, LY=16), and double/no-delay at
2,000,000 clocks (x=27..28, LY=98). The first and last occur after six and eight
completed frames respectively, so initial presentation cannot explain them.
Their cause remains unqualified. The new probe exits 1 for the recorded
14/20 result; a successful capture or repeatable mismatch never qualifies
parity. CPU/PPU timing and capture contents are unchanged in this investigation.
Next isolate pixel-emission and palette-write phases with authored, steady-state
controls, retaining first-frame presentation as a separate witness before
revisiting the commercial mismatches. No release or baseline update is justified
by these controls alone.

The follow-up `cgb-pixel-frontier.json` expands this to 238 paired visible-row
samples over 34 authored ROMs. Thirty-two first-frame controls vary native
speed, all SCX fine-scroll values and marker red=1/2. Both colors are written
before LCD enable. Every sample remains before the first normal VBlank, so the
VBlank palette handler has not executed. Two steady-state controls retain the
earlier normal/speed-switched marker transitions. Seven fixed absolute clock
targets per ROM are repeated twice per core (952 captures). Actual elapsed
clocks, LY and completed-frame counts agree in every paired sample.

Forty-four rows match because neither core has emitted a pixel yet. Four
samples show a one-pixel reference lead where only the reference has started
drawing; 190 show a two-pixel lead. Both initialized colors produce identical
frontier positions within each core. All fourteen steady-state samples retain
the two-pixel lead. Thus active palette writes, first-frame presentation at
VBlank and a particular fine-scroll setting are unnecessary to reproduce this
raw framebuffer difference. These observations do not establish which core
matches hardware or separate LCD-write, line-start and pixel-output phases.

An isolated experiment changed only native CGB `startup_delay_` from 12 to 10
dots. It retained the 44 matching rows, but all other 194 still differed by
one pixel. It also failed the existing CGB contract with `Save state contains
invalid PPU timing state`: the earlier transfer completion produces timing
outside the current serializer's accepted range. The PPU timing contract alone
passed, illustrating why a single focused contract is insufficient. This
candidate was rejected without changing the serializer to accommodate it.
The public patch and required base hash preserve the experiment; production
source, the normal build library and private baselines were never modified.

The next target is to measure first pixel, last pixel and mode-3 VRAM/palette
access boundaries in one authored witness. That can distinguish raw output
phase from CPU-visible transfer length before attempting another correction.
The original fifteen private screenshots remain unqualified and release
publication remains on hold. A closer pixel frontier alone cannot qualify
save-state behavior or the release.

The combined `cgb-pixel-access.json` witness now measures actual executed
VRAM and BG-palette reads/writes alongside the pixel frontier and CPU-visible
STAT mode. It covers native CGB-E at both speeds, ordinary line-one mode-3
entry/exit, eleven phases per boundary and four access operations (176 ROMs).
The tile and palette color under test are unused by the constant red marker.
Read results come from CPU A after the instruction; write acceptance comes
from the raw stored byte. Host reads never attempt the tested write. All
clocks are normalized 8-MHz values relative to the same PC=1000 boundary.

An initial quiet-NOP calibration failed closed: intermediate reference LY/STAT
reads forced incremental rendering instead of its unobserved batched path.
At normal/end/VRAM-read pad170, final registers, output frontier and visible
bytes agreed, but three copied serialized bytes differed (`fetcher_y`,
`last_tile_data_address` and `last_tile_index_address`). These renderer fields
are not excluded from the state comparison or assumed harmless. The owned ROM
builder and failed calibration fingerprints preserve this counterexample.

The accepted measurement protocol uses ROM-authored STAT reads at equal cost
to three NOPs, preventing whole-transfer batching in both snapshot and end-only
runs. Both forms restore A=91 before the tested access. Each case runs twice
with snapshots and once end-only in each core, then repeats the executed-access
check with equal-duration NOP padding and no intermediate host snapshots.
All 176 cases repeat and retain identical final serialization within each
core (only the unused reference host RTC timestamp is masked in the copied
buffer). Every NOP control returns the same access result as its polling form.
This is 1,408 executions, with no production CPU/PPU changes.

Before the native access correction, the cores agreed on 166 executed results
and differed on ten. Each entry
below represents a read/write pair:

| Speed | Boundary | Access | GBB | Pinned reference |
| --- | --- | --- | --- | --- |
| Normal | Mode-3 entry | VRAM | blocked | allowed |
| Double | Mode-3 entry | VRAM | blocked | allowed |
| Double | Mode-3 entry | BG palette | blocked | allowed |
| Normal | Mode-3 exit | BG palette | allowed | blocked |
| Double | Mode-3 exit | BG palette | allowed | blocked |

For example, the normal VRAM-read pair executes over clocks 1088..1104.
GBB returns FF; the reference returns the initialized A5. The normal palette
write at 1432..1456 stores 91 in GBB but retains 03 in the reference, even
though both report mode 0 at the ending snapshot. The double-speed palette
exit pair occurs at 1420..1432. These are executed instruction results, not
inferred access rules based only on a sampled STAT mode. First/last pixel
brackets are retained alongside them; their instruction-sized intervals must
not be represented as exact per-dot edges.

All full-window pixel and mode timelines still differ somewhere. The paired
CLI therefore exits 1, independently of the 166 matching access results.
Matching the reference does not alone establish hardware correctness. The next
implementation target is native VRAM-entry and BG-palette entry/exit sampling,
with read/write symmetry, speed and save/load controls retained. Object-palette
accesses, other CGB revisions and broader fine-scroll/window/sprite effects
remain outside this witness. The fifteen private release screenshots and
publication hold are unchanged; no baseline refresh was performed.

The retained correction resolves those ten executed-access differences without
changing the pixel pipeline or interrupt dispatch. Native CGB CPU VRAM reads
and writes still reach the selected bank at the ordinary dot-80 handoff, while
direct/debug and compatibility arbitration keep their existing rules. CPU
FF69/FF6B data accesses now sample before their last machine cycle; all four
clocks of that cycle remain in the instruction. Normal-speed palette entry
also respects the internal transfer gate before STAT's visible mode bits
change; double speed retains the visible-mode gate at this earlier sampling
phase. Blocked writes still increment and wrap their index. Diagnostic IO
writes remain recorded exactly once at the actual CPU access phase.

The new full paired run matches 176/176 executed BG-palette/VRAM results on
CGB-E, with repeated snapshots, identical end-only serialization and equal-cost
NOP access controls. Additional end-only model substitutions match 176/176 each
on CGB0 and CGB-C (528 total paired access cases). The frozen fixture retains
the previous source revision, initial ten mismatches and original summary.
Local CPU witnesses add both BG/OBJ palette ports, indices 2/63, both speeds,
entry/exit points and full serialized replay on CGB0/CGB-C/CGB-E/the CGB alias,
plus the AGB0/AGB startup-compatibility profiles (600 native witnesses).
Separate monochrome/compatibility witnesses preserve their handoff behavior.
The independent executed-access grid directly measures BG data; OBJ uses the
shared palette gate and has local CPU/save-state coverage rather than a separate
independent matrix. No persistent state field or save-state version is added.

The 4,928-case model matrix reports zero new regressions. All 100 private ROMs
complete their 300 captures, with every pixel hash and capture metadata row
identical to the previous core. This preserves the existing independent result
of thirty exact reference matches and fifteen unqualified screenshots across
ten ROMs among the forty-five baseline changes. The twenty elapsed-clock and
238 pixel-frontier paired rows also remain identical after rerunning their
complete grids; their earlier source hashes are retained explicitly.
All 274 selected public headless contracts pass with no skipped tests. The
expanded native/compatibility unit contract also passes; relinking it against
the previous CPU/PPU/bus implementation fails 504 access/IO-trace assertions.

An additional end-only startup sweep shifts the authored schedule by the
shortened 452-dot first line, using `startup_scenarios()` ROMs in CGB-E.
Before/current/reference runs total 528 executions over 176 cases. The initial
core matched 166; the ordinary-line correction matched 174. The remaining pair
was normal-speed VRAM read/write at startup pad12 (dot 80): GBB returned FF or
retained A5 where the reference read A5 or stored 5A. These differences predated
the ordinary-line correction; its historical results remain in the fixture.

A separate CPU startup exception now allows those two accesses at normal
speed, while retaining double-speed blocking, direct/debug arbitration and
ordinary-line behavior. All 176 startup access results now agree on CGB-E;
additional end-only substitutions match 176/176 each on CGB0 and CGB-C.
The local contract adds 144 startup witnesses over both banks, both speeds,
dots 79/80/81, reads/writes and serialized replay on all six native profiles.
Six additional DMG/MGB/CGB compatibility controls retain startup blocking.
Disabling only the new startup exception makes 24 native assertions fail.
These are startup-compatibility checks for the AGB profiles, not independent
AGB hardware validation. CPU clocks and the renderer remain unchanged.
The repeated startup snapshots equal each core's end-only serialized state,
and all 176 equal-cost padding controls agree. After this startup correction,
the 4,928-case hardware matrix still has zero new regressions; all 300 private
captures and their metadata, twenty elapsed-clock rows and 238 frontier rows
remain identical to the preceding correction. All 274 selected public
headless contracts pass again with no skipped tests.

The mode/pixel timelines are still different, so the combined research CLI
continues to exit 1 despite matching every executed access. The release remains
on hold with its baseline unchanged. Next isolate ordinary-line
pixel-emission phase while retaining the matching access gates as constraints. Moving output or shortening mode 3
cannot be justified by the access results alone.

The follow-up `cgb-pixel-phase.json` brackets first/last pixels and STAT
mode-3 entry/exit on startup line zero and ordinary line one. Eighty owned
ROMs vary both CPU speeds, all eight background fine-scroll values, two
post-enable polling phases, left/middle windows, and left/middle transparent
objects with OBJ display enabled or disabled. Background and window controls
have empty OAM; all setup happens while LCD is off. The unused tile/palette
sentinels and red/white frontier encoding remain unchanged. These are pixel
and STAT observations, not additional executed VRAM/palette access sweeps.

All eighty cases repeat and preserve complete end-only serialization within
each core. Four equal-cost LDH-versus-HL STAT polling controls also preserve
LY/mode/frontier at all 242 shared clock snapshots, with repeat and end-only
checks. The full protocol takes 492 executions using the existing bounded
adapters and pinned SameBoy 1.0.3. Edge intervals are open-left/closed-right
in elapsed 8-MHz clocks, with no fitted offset. Frontier differences use only
joint samples on the same visible line, with both cores actively drawing.

For all sixteen empty-OAM background cases at normal speed, the reference
leads by four interior pixels on startup and two on line one. The sixteen
double-speed cases show leads of two and zero respectively. This does not
establish a universal pixel delay: first-line behavior and CPU speed must be
kept separate, and the earlier elapsed-clock/steady-state controls remain
independent constraints.

The object controls isolate two further disagreements. In all sixteen paired
controls, reference startup edge records with an enabled transparent object
equal empty-OAM records; GBB's do not. On ordinary line one, all sixteen
reference disabled-OBJ edge records equal enabled-OBJ records. GBB instead
matches empty OAM in all sixteen. This supports investigating native CGB
object fetches separately from display enable and from startup OAM selection;
it is software-reference evidence, not independent hardware validation.

No complete pixel or mode timeline agrees across both lines in any of the
eighty cases. The new research CLI therefore exits 1; malformed/incomplete or
observer-dependent measurements exit 2. Public contracts check the retained
disagreements, live two-line edges, polling-form controls, provenance and
fail-closed validation. Production timing, saved state and the private release
baseline were unchanged by that initial measurement. The executed-access
follow-up below isolates native OBJ-disabled fetch scheduling.

The executed-access follow-up, `cgb-object-access.json`, tests 512 owned CGB-E ROMs:
both native CPU speeds, left/middle transparent objects, SCX 0/7, VRAM reads
and writes at eight ordinary-line completion dots (244..272), and four display
histories. These keep OBJ off/on, disable it during the fetch window, or enable
it after the fetch. Equal-cost ROM-authored STAT polls preserve a common clock
schedule. Actual access bytes are compared, including reference boundary reads
of `00`; that value is retained rather than classified as a normal accepted
or blocked sentinel. Two observed takes and an end-only take per core check
complete serialized-state equivalence, for 3,072 executions.

The original implementation matched 402/512. A broad candidate also disabled
native OBJ-off cancellation and matched 488/512, but introduced four new
double-speed read/write disagreements at dot 264. It was rejected. The public
patch, base-source hashes and four counterexamples preserve this experiment;
it is not part of the build. Removing the cancellation behavior without also
resolving the transfer boundary is not a validated correction.

The retained change is narrower: ordinary native CGB fetch scheduling and
object-data gathering no longer depend on display enable. Monochrome and CGB
compatibility arbitration, startup selection, and OBJ-off cancellation remain
unchanged. It matches 466/512, fixing 64 access results without a newly
mismatching case. The four histories now match 122/128 (off), 122/128 (on),
100/128 (disable-during) and 122/128 (enable-after). The remaining 46 results
remain explicit disagreements, so the access research CLI still exits 1.

The repeated 80-ROM pixel-phase sweep now has identical ordinary-line
enabled/disabled edge records within each core for all sixteen object
controls. Its original observations and source hashes remain under
`historical_before`; startup and cross-core pixel/mode differences remain
unqualified. Local native lifecycle witnesses cover six profiles, both speeds,
left/middle objects, six ordinary-line dots, pending-fetch save/load replay,
late enable and warm reset (144 pairs). Their startup histories are identical
to isolate ordinary-line behavior. The previous fetch implementation fails
240 assertions, including 24 colored-cache witnesses covering both VRAM banks
and mid-sprite late enable. Removing only hidden data gathering also fails all
24 colored-cache checks. No persistent state field or version changes.
AGB profiles provide startup compatibility coverage, not independent AGB
hardware validation.

The 4,928-case hardware matrix has zero new regressions. All 100 private ROMs
complete 300 captures with identical pixels and timing metadata. The 176
ordinary access, 238 pixel-frontier and twenty elapsed-clock paired rows remain
identical after rerunning the complete grids. Their earlier source hashes are
retained explicitly. All 276 selected public headless contracts pass with no skipped tests.
The original release hold and private baseline remain unchanged.

Next isolate the native object-transfer boundary, including `00` readback and
the double-speed dot-264 gate, before changing cancellation behavior. A closer
framebuffer or a larger total match count cannot justify those four regressions.

The next `cgb-vram-tail.json` sweep separates stored CPU data from fetch-side
readback. It contains 496 owned CGB-E ROMs and 2,976 executions: both speeds,
both CPU VRAM banks, stored sentinels A5/3C, CPU addresses 8100/8201, background
tile byte pairs 0000/AA55/FFFF, two palette indices and both background banks.
Additional controls fill the unfetched bank with high byte 96 and vary SCY
0/3/7 with row-coded tile bytes and tile two at off-screen map column 20.
Normal-speed accesses complete at scheduled dots 260/264/268; double speed
adds 262/266. These use the previous authored instruction schedule, with
actual elapsed 8-MHz instruction clocks validated in both cores.

Every background color is the same red marker. LY/mode/frontier timelines
remain identical within all sixteen data-variation timing groups, including
across the 448 basic and 48 row/prefetch measurements. All ROM bytes and traces
were revalidated against the final generator before freezing the joined
evidence. Each observed take repeats and equals its end-only serialized state;
the host observation does not change emulated state.

At the tested normal-speed dot-264 boundary, the reference returns the byte
at the background prefetch high-plane address in the CPU-selected VRAM bank,
instead of either requested CPU address's stored sentinel. AA55 tile data
returns 55 when the CPU and background select the same bank. Filling the
other bank with 96 makes a CPU read from that bank return 96, rather than a
fixed cross-bank zero. Palette selection does not change these results.
Row-coded data follows background rows 1/4/0 for SCY 0/3/7. Replacing map
column 20 with tile two selects D2/E3 according to the CPU bank, distinguishing
the off-screen prefetch from the last visible tile. This is evidence for an
address collision, not a universal constant-zero blocked-read rule.

All tested double-speed dot-264 reference reads remain FF despite the varied
data. An FF result alone is ambiguous: normal-speed FFFF data also returns FF
at the collision. The frozen record classifies numerical FF separately from
stored/other data and does not automatically call every FF read blocked.
GBB still returns its stored byte at both dot-264 boundaries. The new grid
therefore matches 336/496 and retains 160 disagreements. The earlier, distinct
512-case object-access grid remains at 466 matches; production is unchanged.

The headless contract checks live bank/address/row controls, complete-state
observer equivalence, the retained reference correlations and rejection of
unexpected timing changes or incomplete measurements. All six related CGB
probe contracts pass. The research CLI still exits 1 for cross-core data
disagreements and 2 for invalid or observer-dependent evidence. These are
pinned SameBoy 1.0.3 CGB-E observations, not independent hardware validation.
Next model and validate this address collision and the double-speed gate
across broader transfer phases and models before changing cancellation.
Saved-state layout, private baselines and the release hold remain unchanged.
