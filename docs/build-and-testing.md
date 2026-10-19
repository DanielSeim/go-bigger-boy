# Building and testing

This guide contains the development, conformance, fuzzing, sanitizer, and
frontend validation workflows. Run commands from the repository root unless a
command explicitly changes directory.

## Native build

```sh
cmake -S . -B build
cmake --build build
ctest --test-dir build --output-on-failure
```

Use CMake 3.20 or newer and a C++17 compiler. Tests and optional SDL3
discovery are enabled by default; without SDL3 the native headless tools and
core tests still build. Use `-DGAMEBOY_BUILD_SDL=OFF` for an explicit headless
build. Multi-configuration generators need `cmake --build build --config
Release` and `ctest --test-dir build -C Release --output-on-failure`.

Full public SGB firmware matrices run in dedicated Linux CI shards; platform
and sanitizer jobs retain the shorter contracts. Local CTest defaults include
the full matrices. See [SGB firmware CI](sgb-firmware-ci.md) for coverage and
reproduction commands.

For playback measurements, use `-DCMAKE_BUILD_TYPE=Release`. Supported native
release builds enable cross-module link-time optimization by default via
`GAMEBOY_ENABLE_RELEASE_IPO`; unsupported compiler/linker combinations fall
back with a configure-time message. Debug, web and sanitizer profiles are not
changed. Use `-DGAMEBOY_ENABLE_RELEASE_IPO=OFF` to opt out when debugging build
or linker problems. Android's packaged `RelWithDebInfo` profile also enables
IPO, using Clang full LTO rather than ThinLTO. This does not enable fast-math,
require a newer CPU instruction set or reduce emulated work/audio quality.

## Performance and load testing

Normal native test builds include a deterministic emulation load test and a
focused FPS-metrics unit test. The load test runs a synthetic ROM for 600
frames, exercises CPU, PPU, timer, APU, input, and framebuffer paths, and
prints structured `performance_metric` records for the emulated FPS, cycles,
audio samples, and FPS measurement windows:

```sh
cmake -S . -B build-performance -DCMAKE_BUILD_TYPE=Release
cmake --build build-performance --parallel
ctest --test-dir build-performance -L performance --output-on-failure
```

When SDL3 is available, the same label also runs a headless software-renderer
benchmark over nearest-neighbor 2D, voxel diorama, shape-aware voxel, and
voxel pop-up modes. It reports one `render_performance_metric` record per
mode, including presentation FPS, elapsed time, and voxel mesh sizes. Set
`GBB_RENDER_MIN_FPS` to adjust its conservative catastrophic-regression floor
when testing on unusually slow or virtualized systems.

The benchmark uses an exact render-target cache for unchanged transformed
frames. Set `GBB_RENDER_PERF_DISABLE_CACHE=1` to measure the uncached mesh
path while profiling. The JSON report includes per-stage timings and cache-hit
counts; runtime SDL diagnostics also include these stages when
`GBB_FRAME_TIMING=1` is enabled.

Set `GBB_RENDER_PERF_CAPTURE_DIR` when running the SDL benchmark to write
deterministic `nearest.ppm`, `voxel.ppm`, `voxel_shape.ppm`, and
`voxel_popup.ppm` captures. Desktop CI stores these captures for every
platform alongside the performance report. The native CTest gate compares
them with the `sdl-software` baseline under `tests/visual-baselines/`.
The Web browser smoke test stores the corresponding canvas captures as PNG
artifacts and compares them with the separate `webgl` baseline using a small
one-channel tolerance. Compare a capture with
`scripts/compare_voxel_screenshots.py` when reviewing a backend-specific
visual change; the existing benchmark also checks direct versus cached RGB
output pixel-for-pixel.

Desktop CI also writes `render-performance.json`, evaluates it against the
platform baseline in `tests/render_performance_baseline.json`, and uploads the
raw and summarized reports. Every mode must remain at least 60 FPS and at
least 65% of its reviewed platform baseline; the relative margin absorbs
normal hosted-runner variance while the absolute floor protects the user-facing
frame-rate target. Update a platform baseline only after reviewing several
runs on the affected platform.

The default performance floor is 30 emulated frames per second. Override it
for a slower or virtualized development machine with
`GBB_PERF_MIN_FPS=10`; this changes only the threshold, not the workload or
the deterministic frame-progress checks. Sanitizer builds omit the
wall-clock performance test because instrumentation changes timing too much;
their functional and diagnostic coverage remains enabled.

The SGB-specific benchmark measures core emulation and border composition
separately for DMG, SGB fallback, and transferred-border cases. Its default
combined-frame floor is 60 FPS; `GBB_SGB_PERF_MIN_FPS` overrides that floor
for diagnostic runs. For on-device SGB/voxel timing and the Android gate, see
[SGB validation](sgb-validation.md).

The experimental firmware-driven SGB1/SGB2 host has a separate serial
playback benchmark and exact-audio regression gate. It measures native and
combined audio in one-second windows, requiring 1.4x p05 and 1.2x worst-window
host headroom. The latest clocked-bridge Linux run fails all four headroom
profiles despite exact audio/state output; historical passing runs do not
qualify this implementation. See
[boot validation evidence](sgb-boot-validation.md#playback-baseline-evidence)
and [bounded SGB host](sgb-host.md#repeatable-playback-headroom-gate)
for caller-owned firmware requirements and commands. This does not replace
frontend FPS or on-device validation.

## Fuzzing

Parser/protocol fuzzing is opt-in and requires a Clang toolchain with
libFuzzer. Build it alongside the native sanitizers, then provide a corpus
directory (libFuzzer will create and extend it):

```sh
cmake -S . -B build-fuzz -G Ninja \
  -DCMAKE_CXX_COMPILER=clang++ \
  -DGAMEBOY_BUILD_SDL=OFF \
  -DGAMEBOY_BUILD_FUZZERS=ON
cmake --build build-fuzz --target gameboy_parser_fuzzers gameboy_parser_review
mkdir -p fuzz-corpus
./build-fuzz/gameboy_parser_fuzzers fuzz-corpus -max_total_time=60
```

For the reproducible campaign runner used by CI, keep generated inputs out of
the reviewed seed corpus and run:

```sh
FUZZ_CAMPAIGN_SECONDS=1800 bash tests/fuzz/run_campaign.sh \
  tests/fuzz/corpus fuzz-corpus-run fuzz-artifacts
```

Inspect new corpus files and crash artifacts before promoting any input to
`tests/fuzz/corpus`. The runner treats its timeout as a successful campaign;
libFuzzer crashes and other non-zero exits remain failures.

The checked-in corpus can be validated independently before a campaign. The
check rejects empty, oversized, or duplicate seeds and verifies the reviewed
checksum manifest:

```sh
bash tests/fuzz/check_corpus.sh
```

After reviewing a downloaded campaign corpus, minimize it against the checked
in seeds with a dry run first, then explicitly approve the promotion:

```sh
bash tests/fuzz/promote_corpus.sh fuzz-corpus-run tests/fuzz/corpus
bash tests/fuzz/review_corpus.sh fuzz-corpus-run
# Inspect fuzz-corpus-run.semantic-review.tsv before approving promotion.
bash tests/fuzz/promote_corpus.sh fuzz-corpus-run tests/fuzz/corpus --approve
```

The dry run reports the required review step without minimizing or modifying
the corpus. Approved promotion verifies the semantic review against the input
hashes, uses libFuzzer's merge mode in a temporary directory, copies minimized
inputs into the reviewed corpus, and updates its checksum manifest.

The target feeds bounded inputs through settings, trace, save-state,
link-packet, and SGB parsing boundaries. Fuzzing is deliberately separate from
the normal CTest suite so release and cross-platform builds remain dependency
free.

## Sanitizers and frontend smoke tests

The CI ThreadSanitizer job also builds the SDL frontend with SDL enabled,
runs the display-independent contract tests, and performs a bounded dashboard
window and debugger smoke under Xvfb, driven by `tests/sdl_tsan_smoke.sh` with a
synthetic ROM and checked debugger captures. Longer interactive sessions
remain part of the normal desktop build because they require a display server
and user input.

To reproduce that check locally on a native Linux host with SDL3 installed:

```sh
cmake -S . -B build-tsan-sdl \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DGAMEBOY_BUILD_TESTS=ON \
  -DGAMEBOY_BUILD_SDL=ON \
  -DGAMEBOY_ENABLE_THREAD_SANITIZER=ON
cmake --build build-tsan-sdl --parallel
TSAN_OPTIONS=halt_on_error=1:second_deadlock_stack=1 \
  ctest --test-dir build-tsan-sdl --output-on-failure --parallel 2
```

For the bounded dashboard input smoke, install Xvfb and xdotool, then run:

```sh
SDL_AUDIODRIVER=dummy SDL_VIDEODRIVER=x11 WAYLAND_DISPLAY= \
  SDL_RENDER_DRIVER=software \
  TSAN_OPTIONS=halt_on_error=1 \
  bash tests/sdl_tsan_smoke.sh ./build-tsan-sdl/gbb
```

Pass a synthetic ROM path as the second argument to exercise debugger input;
CI checks the resulting captures with `tests/check_desktop_debugger_captures.py`.
The helper retains logs under `tsan-gui-artifacts/` and interprets its bounded
timeout. A raw `timeout` command alone is not a clean-shutdown assertion.

The Web frontend has a browser-level smoke that loads the real WebAssembly
bundle, changes the persisted display/audio settings, reloads the page, and
opens a small synthetic ROM. CI runs it with headless Chromium. With Node.js
and Playwright available locally:

```sh
# In one terminal; leave this server running while the tests execute.
python3 -m http.server 8765 --bind 127.0.0.1 --directory build-web/web
# In another terminal, from the repository root:
npm install --no-save --no-package-lock @playwright/test@1.52.0
npx playwright install chromium
npx playwright test \
  --config=tests/web/playwright.config.mjs tests/web/frontend.spec.mjs
```

Build the bundle first with `scripts/build-web.sh`. Set `GBB_WEB_BASE_URL`
to test a different server and `GBB_WEB_CAPTURE_DIR` to retain voxel PNGs.
These setup commands can download packages and browsers; use an existing local
installation for an offline run. CI also applies the Web visual regression gate.

Android UI coverage runs the same library-to-settings flow through Espresso on
an emulator. The workflow invokes `connectedDebugAndroidTest`; locally use:

```sh
(cd android && ./gradlew connectedDebugAndroidTest)
```

## CLI and conformance tools

Inspect a ROM and execute a requested number of starter instructions:

```sh
./build/gbb_cli path/to/game.gb 10
```

Export the current hardware scene as machine-readable JSON after optionally
executing instructions:

```sh
./build/gbb_cli path/to/game.gb 10 --scene-json scene.json
```

The file uses the versioned `gbb.scene.v1` schema and includes display
registers, background/window tile maps, decoded tile graphics and palettes,
plus OAM sprite metadata. The same schema is available through
`gbb::scene_snapshot_to_json` and the web frontend's **Export scene JSON**
button, allowing external renderers and analysis tools to consume snapshots
without linking against emulator internals.

Run an individual acceptance-test ROM with a bounded cycle budget:

```sh
./build/gbb_test_runner path/to/test.gb --max-cycles 100000000
```

The runner recognizes Mooneye's `LD B,B` result protocol and serial test output
containing `Passed` or `Failed`, plus Blargg's `$A000` memory result protocol.
GBMicrotest's HRAM result protocol (`FF80`/`FF81`/`FF82`) is available through
`--protocol gbmicrotest`. The wilbertpol extension uses its `0xED` completion
opcode via `--protocol mooneye-wilbertpol`. Use `--protocol mooneye`,
`--protocol serial`, or `--protocol blargg` to disable automatic protocol
detection. Model-specific post-boot tests can select
`--model auto`, `dmg0`, `dmg`, `mgb`, `sgb`, `sgb2`, `cgb0`, `cgb-c`, `cgb-e`,
`agb0`, or `agb`. The AGB profiles provide startup compatibility with the
CGB-E runtime baseline, not native GBA emulation.
The historical `cgb` spelling remains accepted as the late CGB-E profile.
For boot-path diagnostics, add `--diagnostic-boot`; this runs GBB's original
diagnostic boot ROM, records a handoff marker in HRAM, validates the CPU
handoff state, and then continues at the cartridge entry point. Normal
emulator construction continues to use the existing post-boot path.
For the bundled original cold-start replacement, use the historical CLI flag
`--dmg-boot` with the desired model, for example `--dmg-boot --model dmg`.
Despite its name, this flag selects the model's generic replacement path.
It initializes hardware through emulated CPU writes; validation and handoff
details vary by model. Its branded presentation is not Nintendo artwork.
See [replacement firmware](../firmware/README.md) for
rebuilding and testing it; RGBDS is not required for normal builds.

The recorded APU baseline passed all 12 upstream Blargg `dmg_sound` tests and all 12 `cgb_sound`
tests, including model-specific power behavior, active wave-RAM access, and the
original DMG hardware's channel 3 retrigger corruption.
The recorded headless accuracy baseline passed all 75 Mooneye acceptance ROMs,
all 6 applicable CGB misc ROMs, all 28 emulator-only mapper ROMs, 38 curated
Blargg ROMs, and 21 exact Acid2/Scribbltests/Mealybug/Gambatte framebuffer
comparisons. The separate [hardware-model matrix workflow](https://github.com/DanielSeim/go-bigger-boy/actions/workflows/hardware-model-matrix.yml)
evaluates additional GBMicrotest and Mooneye-wilbertpol cases. The desktop
workflow also publishes a research report for AGE, SameSuite and BullyGB: it discovers
AGE screenshot references, applies its DMG-compatibility color rules, maps
SameSuite diagnostics to documented hardware profiles, and keeps exploratory
failures outside the release gate (see the [accuracy report](accuracy.md)).
Each run publishes the full Markdown reports as downloadable artifacts.
These counts preserve the measured baseline; confirm the current registered
cases in `cmake/ConformanceTests.cmake` and the report for the candidate commit
rather than treating them as a fresh run.

To register the pinned v7.0 bundle locally, download and extract
`c-sp/game-boy-test-roms`, then set its root as the opt-in cache path. The
test ROMs are deliberately not bundled or downloaded by the build:

```sh
cmake -S . -B build-conformance \
  -DGAMEBOY_TEST_ROM_DIR=/path/to/game-boy-test-roms-v7.0
cmake --build build-conformance
ctest --test-dir build-conformance -L conformance --output-on-failure
```

To run the metadata-driven AGE, SameSuite and BullyGB research harness locally, add
`-DGAMEBOY_ENABLE_EXTERNAL_SUITES=ON` to the configure command and run:

```sh
ctest --test-dir build-conformance -L external-suites --output-on-failure
```

The report is written to `external-suite-report.md`. Research failures are
reported for review but do not fail unless a suite entry is explicitly
promoted to the `required` gate.

GBVisualizer is documented as an [external CGB reference candidate](gbvisualizer-reference.md)
for PCM12/PCM34, double-speed, timer and scanline integration. It is not yet a
qualified pixel gate or bundled test ROM. The guide includes a pinned external
builder, CGB SameBoy capture adapter and opt-in native tests, plus the distinction
between CPU-cycle and PPU-frame checkpoints at double speed. The offline tooling
contract is registered as `gameboy_gbvisualizer_reference_tools_contract`.
The authored normal/double-speed LCD/VBlank marker probes have their own offline
`gameboy_cgb_frame_anchor_probe_contract` and optional paired native tests;
see the same guide for the reported first-frame distinction.
Isolated timer/PCM and STAT/SCX comparisons are covered by
`gameboy_cgb_pcm_stat_probe_contract`, with optional paired native tests and an
authored pixel oracle; the guide records the tested cases and remaining combined
interrupt coverage.
`gameboy_cgb_combined_irq_probe_contract` checks the authored combined producer/
consumer and VBlank-update probes. Their paired comparison currently reports PCM
and pixel differences; a passing tool contract does not qualify those results as
reference parity. See the guide for the first timer-phase divergence.
`gameboy_cgb_irq_timing_probe_contract` covers shorter isolated/mixed event logs,
matched-cost SCX controls and IME-disabled STAT polling. Its measured normal-speed
pending-timer distinction and separate double-speed observations remain research
findings, documented in the same guide.
`gameboy_cgb_lcd_startup_probe_contract` checks a separate authored LCD-enable
producer, request polling versus IRQ acknowledgement, speed witnesses, and frozen
two-event logs. Its optional paired reference run is:

```sh
python3 scripts/cgb_lcd_startup_probe.py \
  --runner build/gbb_test_runner --sameboy-capture /path/to/sameboy-capture \
  --output /path/to/new-lcd-startup-results
```

Build the adapter against the pinned SameBoy Core as described in the
[reference guide](gbvisualizer-reference.md). The probe supplies the original
GBB boot image and authored cartridges; no proprietary inputs are needed.
Twenty-four cartridges cover VBlank/STAT, polling/IRQ, both speeds and three
pre-enable NOP delays. Frame checkpoints 4 and 8 must contain the same frozen
WRAM log, with two takes per core. These are completed PPU-frame checkpoints,
not `--cycle-frame-capture` intervals. The retained pre-correction evidence
matched 42/48; native interrupt-fetch recognition now matches all 48 and exits
0. See [accuracy](accuracy.md) for the measured scope and limits.

`gameboy_cgb_stat_request_phase_probe_contract` checks the finer first-line
IF/mode/LY read sweep and LCD-off software-STAT-request controls. Run the paired
measurement with the same runner/adapter arguments and a new output directory:

```sh
python3 scripts/cgb_stat_request_phase_probe.py \
  --runner build/gbb_test_runner --sameboy-capture /path/to/sameboy-capture \
  --output /path/to/new-stat-request-phase-results
```

Its 86 comparisons repeat at completed PPU frame 4. All now match, after
correcting split double-speed peripheral-clock accounting. This does not
qualify the separate normal-speed LCD-startup IRQ counter discrepancy above.


`gameboy_cgb_stat_vector_probe_contract` checks terminal first-vector witnesses,
stacked return-PC instruction boundaries and fail-closed result handling. Run:

```sh
python3 scripts/cgb_stat_vector_probe.py \
  --runner build/gbb_test_runner --sameboy-capture /path/to/sameboy-capture \
  --output /path/to/new-stat-vector-results
```

The 64 authored cartridges cover hardware STAT versus LCD-off software IF,
two foreground loops, eight post-trigger NOP phases and both CPU speeds.
Two captures per core at completed PPU frame 4 must repeat. The paired run
now matches all 64 comparisons and exits 0. Historical 48/64 results remain
recorded separately. The offline contract validates tooling, while the paired
run records the measured architectural agreement.


`gameboy_cgb_stat_boundary_read_probe_contract` checks a complementary IF-read
sweep with IME disabled and IE still set to 2. Run:

```sh
python3 scripts/cgb_stat_boundary_read_probe.py \
  --runner build/gbb_test_runner --sameboy-capture /path/to/sameboy-capture \
  --output /path/to/new-stat-boundary-read-results
```

The grid covers both foreground patterns, eight post-LCD NOP phases, three
whole-loop counts near the 452-dot first-line boundary, every instruction slot
and both speeds: 288 cartridges, two takes per core at completed PPU frame 4.
Taken branches use a zero displacement for finite unrolling, preserving their
cycle cost and register/flag effects. The primary IF read starts after that
prefix; it does not measure IF instantaneously at the preceding boundary.
The paired run matches all 288 cases and exits 0. Both pending and non-pending
primary IF reads are covered at each speed, with all 1,152 raw captures checked.
This does not resolve the separate first-vector mismatches.
See [accuracy](accuracy.md) for the measured scope and limits.


`gameboy_cgb_stat_clock_probe_contract` exercises the standalone
`gbb_stat_clock_trace` observer in the headless build, validates the bounded
trace schema and checks observer-disabled controls. It needs no external core.
The paired research run additionally needs the separate
`scripts/sameboy_stat_clock_trace.c` adapter. Unlike the existing public-API
capture adapter, this tool reads SameBoy's **internal ABI** for callback clocks.
Build it only against the pinned 1.0.3 checkout at
`208ba4afabffab9edde416f2dbb8ae459e34adb8` and its stock full-featured Core library;
its layout guard requires internal struct version 15. Headers and library must
have identical build options. Do not mix `GB_DISABLE_*` configurations.

```sh
cmake -S . -B build-headless -DGAMEBOY_BUILD_TESTS=ON -DGAMEBOY_BUILD_SDL=OFF
cmake --build build-headless --target gameboy_stat_clock_trace
cc -std=c11 -O3 -Wall -Wextra -Werror -Wno-multichar -DGB_INTERNAL \
  -I /path/to/SameBoy scripts/sameboy_stat_clock_trace.c \
  -L /path/to/SameBoy/build/lib -lsameboy \
  -Wl,-rpath,/path/to/SameBoy/build/lib -lm -o /tmp/sameboy-stat-clock
python3 scripts/cgb_stat_clock_probe.py \
  --gbb-clock-trace build-headless/gbb_stat_clock_trace \
  --sameboy-clock-trace /tmp/sameboy-stat-clock \
  --output /path/to/new-stat-clock-results
```

Both adapters take `ROM BOOT [--no-observers]`, consume the same owned boot,
limit execution to 20 million normalized 8-MHz clocks and retain at most 512
steps. A stalled clock or exhausted log fails with exit 3. They write bounded
JSON to stdout and create no saves. The probe stores those bounded records in
its chosen output directory. Request intervals are lower-exclusive and
upper-inclusive observations, not exact PPU edge timestamps. Opcode callbacks
have explicitly different phases in the two cores.

Sixty-six cases cover the prior 64-case vector grid plus a NOP calibration at
each speed. Each core runs twice with observers and once without: 396 traces.
Within each core, final registers, WRAM, normalized clocks and serialized-state
fingerprints must agree with its observer-disabled run. Only the unused
host-initialized RTC timestamp is masked in a **copied** SameBoy serialization,
restricted to these 32-KiB ROM-only cartridges; no emulated state is modified.
The paired result now matches all 66 architectural witnesses and exits 0.
The native IRQ observer records whether recognition occurred after a discarded
fetch. That fetch is included in the 20 entry clocks. The IO access phases
remain unchanged; their internal write-clock separation is still reported.
A disabled-observer run can miss the transient IF rise and acknowledgement
within one entry step; it records that observation as unavailable, not a
fabricated timestamp. See [accuracy](accuracy.md) for the implementation-phase
interpretation and the retained historical results.


`gameboy_cgb_lcd_bus_phase_probe_contract` checks four independently authored
LCD-write forms and three equal-cost IF-read forms. Run the optional measurement:

```sh
python3 scripts/cgb_lcd_bus_phase_probe.py \
  --gbb-clock-trace build-headless/gbb_stat_clock_trace \
  --sameboy-clock-trace /tmp/sameboy-stat-clock \
  --runner build-headless/gbb_test_runner \
  --sameboy-capture /path/to/sameboy-capture \
  --output /path/to/new-lcd-bus-phase-results
```

The write forms are `LDH (a8),A`, `LD (a16),A`, `LD (C),A` and `LD (HL),A`.
Their NOP foregrounds exit through the same terminal IRQ logger. The three read
forms use preceding NOPs to align their memory-read cycle and consume exactly
16 CPU clocks. Thirteen delay phases at both speeds give 78 read comparisons;
52 additional relations require agreement across forms within each core.
Observer-enabled and disabled write runs retain the serialized-state checks.

IO access timing is unchanged: all 78 read cases and eight architectural write
witnesses match. The internal LCD-write separation remains explicit under
`lcd_phase_match`; it is not silently converted into a timestamp match.
Whole-IO and PPU-read phase changes were rejected after a private counterexample
and a DMA snapshot contract failure. Narrower alternatives regressed previously
matching STAT/LY controls. The code under
`tests/fixtures/research/*phase-rejected.patch` is **rejected research**, excluded
from every build. These patches disable the retained fetch-recognition fix in
an isolated copy and reproduce the alternative timing experiments; the fixture
records the required base CPU source hash. They must not be added to production.
Passing a focused research CLI does not qualify release parity.

Elapsed-clock framebuffer comparisons use the separate public-API adapter:

```sh
cc -std=c11 -O3 -Wall -Wextra -Werror -I /path/to/SameBoy \
  scripts/sameboy_elapsed_capture.c -L /path/to/SameBoy/build/lib -lsameboy \
  -Wl,-rpath,/path/to/SameBoy/build/lib -lm -o /tmp/sameboy-elapsed-capture
/tmp/sameboy-elapsed-capture owned.gbc owned-boot.bin cgb-e \
  900000,900128,900256 /path/to/new-output-prefix
```

It anchors clocks at boot unmap with PC=0100, counts only normal VBlanks, and
synchronizes deferred PPU work before consuming pixels via a public LY read.
No additional CPU instruction, frame or fitted offset is introduced. Metadata
records `ppu_synchronized`, `pixels_flushed`, requested and actual clocks.
An owned-ROM eight-target calibration reproduces 47 stale pixels before the
flush, with identical CPU clocks and legacy metadata; repeated images agree.
The adapter accepts at most eight positive increasing targets and refuses
existing output files. Original boot images are external private inputs when
used for commercial comparisons; the calibration uses only the owned boot.
See [accuracy](accuracy.md) for current results and release limits.

The headless test build also provides `gbb_elapsed_capture`, a native CGB-E
research adapter. It accepts `ROM owned|post TARGETS_8MHZ OUTPUT_PREFIX`, anchors
its normalized clock at PC=0100 after boot unmap, and writes the existing raw
framebuffer without changing presentation or PPU state. It bounds boot and
capture execution to 200 million 8-MHz clocks each, accepts at most eight
increasing targets, and refuses existing images. It reads cartridge bytes
directly, so adjacent battery saves cannot affect the comparison or be written.
An instruction may overshoot a target; actual clocks remain explicit.

```sh
cmake --build build-headless --target gameboy_elapsed_capture
python3 scripts/cgb_elapsed_anchor_probe.py \
  --gbb-capture build-headless/gbb_elapsed_capture \
  --sameboy-capture /tmp/sameboy-elapsed-capture \
  --output /path/to/new-elapsed-anchor-results
```

This paired calibration uses four authored ROMs, the same owned boot, two takes
per core and five fixed targets, covering both CPU speeds and delayed LCD enable.
It preserves complete metadata and image hashes in its report and exits **1**
for any pixel mismatch. The recorded result is 14/20 exact images, with six
explicit disagreements despite equal LY and completed-frame counts. This is
research evidence, not a passing release gate. Public CTest checks the capture
adapter, disabled LCD, target bounds, overwrite protection, split-capture
equivalence and frozen evidence without requiring SameBoy or private inputs.

`gameboy_cgb_pixel_frontier_probe_contract` adds first-frame and steady-state
pixel-prefix controls using the same capture adapters:

```sh
python3 scripts/cgb_pixel_frontier_probe.py \
  --gbb-capture build-headless/gbb_elapsed_capture \
  --sameboy-capture /tmp/sameboy-elapsed-capture \
  --output /path/to/new-pixel-frontier-results
```

Thirty-two first-frame ROMs cover both speeds, all eight SCX fine-scroll values
and two colors initialized before LCD enable. They capture before the first
VBlank, so their handler has not executed and no palette is written with the
LCD on. Two existing steady-state ROMs retain their VBlank color markers.
Seven fixed clock targets per ROM, repeated twice per core, produce 952 images
and 238 paired visible-row witnesses. The extra `LD A,SCX` costs eight CPU
clocks; the authored schedule includes that cost identically in both cores.
No reference clock is shifted to fit GBB. The probe rejects differing clock,
LY or completed-frame anchors and rows containing anything other than the
expected current-color prefix and previous-color suffix.

The recorded CLI exits **1**: 44 rows match, four differ by one pixel at the
start of output, and 190 differ by two pixels. Preloaded color changes preserve
the frontier positions in both cores. The frozen fixture is unqualified
research evidence. An isolated native-CGB startup-delay reduction from 12 to
10 dots leaves 194 one-pixel disagreements and fails the CGB save/load contract.
Its patch under `tests/fixtures/research/cgb-pixel-startup-rejected.patch` is
excluded from all builds; its base source hash and rejection are recorded.
See [accuracy](accuracy.md) for the remaining interpretation limits.

`gameboy_cgb_pixel_access_probe_contract` combines pixel output with executed
VRAM and BG-palette accesses on native CGB-E. The headless build supplies
`gbb_pixel_access_trace`; build the optional pinned SameBoy 1.0.3 adapter:

```sh
cc -std=c11 -O3 -Wall -Wextra -Werror -Wno-multichar -DGB_INTERNAL \
  -I /path/to/SameBoy scripts/sameboy_pixel_access_trace.c \
  -L /path/to/SameBoy/build/lib -lsameboy \
  -Wl,-rpath,/path/to/SameBoy/build/lib -lm -o /tmp/sameboy-pixel-access
python3 scripts/cgb_pixel_access_probe.py \
  --gbb-access-trace build-headless/gbb_pixel_access_trace \
  --sameboy-access-trace /tmp/sameboy-pixel-access \
  --output /path/to/new-pixel-access-results
```

Each adapter accepts `ROM BOOT [--end-only]`, restricts inputs to authored
32-KiB native CGB ROM-only images and a 256-byte owned boot, and observes the
PC=1000..1100 window. Execution is bounded to 20 million 8-MHz clocks and 512
window instructions; at most 513 snapshots are retained. No battery files are
loaded or written. The reference uses pinned internal ABI 15 only for raw
VRAM/palette reads and masks the unused host RTC epoch solely in a copied
serialization, as with the earlier STAT observer. LY reads synchronize its
deferred pixels before inspection. First/last pixel observations are bounded
instruction-clock intervals, not exact hardware edge times.

The 176 ROMs sweep VRAM/BG-palette reads/writes at mode-3 entry/exit, at both
CPU speeds. They access an unused tile and unused palette color; the red pixel
marker remains intact. ROM-authored STAT reads replace three NOPs at equal
12-clock cost, with the write operand restored before each tested instruction.
This prevents the reference's batched and incremental render paths from
invalidating the snapshot-state control. Two snapshot runs, one end-only run
and one equal-cost NOP end-only control per core give 1,408 executions.

Every snapshot run repeats and matches its end-only serialized state; every
access result matches the NOP control. Of the cross-core executed access
results, the initial run matched 166 and differed on ten. The retained native
CPU access correction now matches **176/176**, with the initial mismatches
preserved under `historical_before`. End-only model controls also match
176/176 each on CGB0 and CGB-C. Mode and pixel timelines remain explicitly
different; the paired CLI still exits **1**. A rejected quiet-NOP calibration is
preserved because its final visible snapshot agrees while its serialized
reference renderer differs. Those differences are not masked. Public CTest
needs no external library and checks live allowed/blocked accesses, serialized
state equivalence, bounds, invalid records and the frozen paired evidence.
The CGB contract adds both palette ports, auto-increment/wrap on blocked writes,
instruction-clock and IO-trace checks, compatibility controls and serialized
replay at the entry/exit boundaries. The ordinary elapsed-clock and pixel-frontier
grids were rerun unchanged; their full paired rows are identical to the earlier
core, with previous source hashes retained in revalidation metadata.
Additional end-only CGB-E startup controls use ROMs from `startup_scenarios()`
and the same adapters. The 176 before/current/reference comparisons improve
from 166 to 174 matching access results at the ordinary-line correction
stage. A separate startup correction now matches all 176/176, preserving the
old results explicitly. Further startup model controls match 176/176 each on
CGB0 and CGB-C. The CPU permits first-line dot-80 VRAM access only at normal
speed; direct access stays blocked. Both banks and serialized replay have
144 additional local CPU witnesses, and live authored startup ROMs check
observer/end-only equivalence across the VRAM entry boundary. The live
boundary/save-state unit coverage also includes the AGB0/AGB
startup-compatibility profiles; those are not independent AGB sweeps.

`gameboy_cgb_pixel_phase_probe_contract` adds a bounded two-line phase sweep
using the same adapters:

```sh
python3 scripts/cgb_pixel_phase_probe.py \
  --gbb-access-trace build-headless/gbb_pixel_access_trace \
  --sameboy-access-trace /tmp/sameboy-pixel-access \
  --output /tmp/cgb-pixel-phase
```

The output directory must be new. Eighty owned ROMs cover both speeds,
SCX 0..7, two polling phases, left/middle windows and transparent objects with
OBJ display enabled/disabled. All controls initialize OAM while LCD is off;
background/window controls leave it empty. Two observed takes and one end-only
take per core verify complete serialized-state equivalence. Four additional
equal-clock STAT addressing-form controls bring the total to 492 executions.
The frozen `cgb-pixel-phase.json` retains first/last pixel and STAT transition
intervals for both lines, interior frontier differences, source hashes and
reference provenance. First-line behavior, ordinary-line output phase and
OBJ fetch/display behavior remain distinct findings. Exit 1 means a measured
cross-core disagreement; exit 2 means invalid/incomplete or observer-dependent
evidence. Production behavior and private baselines are unchanged.


`gameboy_cgb_reti_dispatch_probe_contract` adds software-requested RETI/RET/EI
phase sweeps and an equal-cost direct-call control. These exposed and verify a
CGB timer-entry clock correction; the guide distinguishes the now-matching
software dispatch cases from remaining combined and demo differences.
`gameboy_cgb_timer_boundary_probe_contract` checks the independent overflow/reload
prediction and foreground/HALT read controls. Its unresolved reload-cycle sampling
distinction is explicitly reported, with bus-phase assumptions and hardware
validation limits documented in the same guide.
`gameboy_mooneye_timer_reference_contract` checks the opt-in pinned external timer
suite's provenance and explicit pass/fail handling. Its five Mooneye tests pass
in both emulators on three CGB revisions; that evidence does not adjudicate the
separate fast-timer HALT read collision. Public CI needs no external checkouts.
The boundary tool also offers `--independent-phases` and `--case` for a foreground
DI phase matrix and minimal reproductions. Its 96 paired foreground cases match;
the original HALT distinction remains separate and unresolved.
`gameboy_cgb_halt_wake_probe_contract` adds HALT/NOP waiting with IME on/off,
direct IF-edge reads and DIV-boundary observations. Busy waiting reproduces the
service-phase discrepancy, while IME-disabled controls agree; the guide records
this refinement without qualifying either emulator's timing.

## Hardware-model matrix

To generate the hardware-revision matrix report, add
`-DGAMEBOY_ENABLE_MODEL_MATRIX=ON` to the configure command and run
`ctest --test-dir build-conformance -L model-matrix --output-on-failure`.
The report is written to `hardware-model-matrix.md`; it keeps reviewed
`EXPECTED_FAIL`/`KNOWN_FAIL` outcomes separate from `REGRESSION` failures. CI
compares the matrix against `tests/model_matrix_baseline.json`: pre-existing
failures remain visible as `KNOWN_FAIL`, while a newly failing ROM/model pair
fails the hardware-model workflow.
Unexpected failures also receive a compact diagnostic bundle under
`matrix-diagnostics/`. Each bundle contains the runner log plus bounded CPU,
PPU, I/O, and APU traces, making interrupt, divider, and rendering timing
failures reproducible from the CI artifact without rerunning the entire matrix.
Individual discovered-suite CTest cases are available with
`-DGAMEBOY_ENABLE_DISCOVERED_CONFORMANCE=ON`; this bring-up mode is
intentionally separate from the normal release baseline.

Desktop ROM-launch errors remain visible until dismissed. On Windows, returning
to the native library waits for the error dialog to close so that hiding the
emulator window cannot hide the message. Launch failures record the requested
ROM, hardware model and exception detail in `logs/gbb.log` below the SDL user
settings directory (`SDL_GetPrefPath("Go Bigger Boy", "GBB")`). This file sink
is enabled in release builds too; error records are flushed immediately. An
explicit `GBB_LOG_FILE` or file-based `GBB_TRACE_WX` sink takes precedence.
Windows Unicode profile paths are supported. If the log cannot be opened,
the UI error remains available and logging falls back to stderr and the bounded
in-memory sink. Log files append across launches.

The headless runner can also capture a deterministic framebuffer without SDL:

```sh
./build-conformance/gbb_test_runner test.gb \
  --model dmg --frames 60 --frame-output capture.ppm
```

For a snapshot after a fixed CPU duration, including games that disable the LCD,
use the independent cycle-based mode:

```sh
./build-conformance/gbb_test_runner test.gb \
  --model dmg --cycle-frame-capture 600 --frame-output capture.ppm
```

This runs until `Cpu::total_cycles()` reaches `N * 70,224` T-cycles from startup
(42,134,400 for 600 intervals), then writes the current framebuffer even without
a PPU frame-ready signal. Capture occurs after the CPU instruction or idle step
that reaches the target, so a small instruction-sized overshoot is possible.
Any selected boot-ROM execution counts toward this duration. At normal speed,
each interval has the duration of a nominal Game Boy frame (about 59.7 Hz).
CGB double speed still counts CPU T-cycles: the PPU advances half as far per
interval. This is a CPU-duration regression snapshot, not a precise PPU-frame
boundary or a speed-normalized clock.

Immediately before writing a cycle capture, the runner emits one
`CAPTURE_DIAGNOSTICS` line on stderr with `cycles`, `pc`, `lcdc`, `stat`, `ly`,
`dot`, `mode`, `completed_frames`, `unique_colors` and `blank_frame` as
space-separated `key=value` fields. PC and the three LCD registers use
zero-padded hexadecimal without a prefix; counts, dot and mode use decimal.
`unique_colors` counts RGB values in the selected output framebuffer, including
the border with `--sgb-frame`. `blank_frame=true` means every pixel has the same
RGB color and still returns success. Diagnostics also work with the LCD off;
`completed_frames` counts actual PPU frames rather than virtual intervals.

For MMM01 mapping, long NOP sequences and the first LCD-off write, optional
`--trace-mmm01 PATH`, `--trace-nop-run N` and `--trace-first-lcd-off` diagnostics
are available. Their fixed histories, output ceilings, write/fetch timing and
Synology commands are documented in the
[MMM01 investigation](mmm01-dump-layout-analysis.md).

For bounded CPU control-flow diagnostics, add either or both options:

```sh
./build-conformance/gbb_test_runner test.gbc --model cgb-e \\
  --cycle-frame-capture 600 --frame-output capture.ppm \\
  --trace-recent-cpu 128 --trace-first-pc 0x0038
```

`--trace-recent-cpu N` retains the last 1–4096 executed instructions in a ring
buffer and emits one `CPU_RECENT` line per retained instruction, oldest first,
immediately before the cycle capture diagnostics. Idle HALT/STOP steps and
interrupt entry do not consume ring entries. The ring uses at most 128 KiB;
output is bounded to 4096 instruction lines, independent of emulation duration.
It is dumped only on a successful cycle-capture path, not on timeout or in other
capture modes. `--trace-limit` does not control this separate ring.

`--trace-first-pc ADDRESS` accepts decimal or `0x` hexadecimal addresses from
0 through 65535. It emits at most one `CPU_FIRST_PC` line when the architectural
PC first equals the address, including in protocol modes. Both line formats
contain `cycle`, `duration`, the previous instruction's original `pc` and actual
fetched `opcode`, and `next_pc`, `sp`, `af`, `bc`, `de`, `hl`. Addresses, opcodes
and register values are zero-padded hexadecimal without prefixes; cycles and
durations are decimal T-cycles. Register snapshots describe the resulting state.
Recent records use instruction-start cycles; first-hit records use arrival
cycles and the duration of the step that reached the address.

First-hit lines additionally distinguish `transition=instruction`, `interrupt`
or `initial`. Interrupt arrival retains the last executed instruction's PC and
opcode, rather than inventing an interrupt opcode. At initial PC arrival there
is no previous instruction (`pc=none opcode=none`, duration zero). A legitimate
`RST 38h` is reported as `opcode=ff next_pc=0038 transition=instruction`; neither
that address nor a white capture is automatically classified as an error.
The optional fetch observer reads the actual CPU opcode, including DMA bus
blocking, without extra emulated reads, clock advances or state writes.

When `--trace-first-pc` reaches its target, the first-hit line is immediately
followed by up to 64 `CPU_FIRST_PC_HISTORY` lines in execution order. This
separate ring exists only with `--trace-first-pc` enabled and is independent of
`--trace-recent-cpu N`. It includes the instruction that reaches the target;
an initial-PC match has no preceding history. HALT/STOP idle steps and interrupt
entry are excluded. Each history entry contains instruction-start `cycle`,
`duration`, `pc`, the fetched `opcode`, `next_pc`, `sp_before`, `sp_after` and
post-instruction `af`, `bc`, `de`, `hl`. The history is emitted once, directly
to stderr, without writing a CPU trace file. It can expose a return or jump to
`0000` followed by a `RST 38h`, including the associated stack changes.

The same first-hit event then emits up to 64 `CPU_STACK_DIAGNOSTICS` lines,
one per retained instruction, in execution order. Each repeats `cycle`, `pc`,
`opcode`, `next_pc`, `sp_before`, `sp_after`, and the post-instruction registers.
`stack_bytes` contains eight CPU-visible bytes starting at the original SP,
with 16-bit address wrapping; `return_address` decodes its first two bytes in
little-endian order. `snapshot=before_step` means these bytes and the DMA/bank
metadata were sampled **before the instruction's opcode fetch**. They are not
a log of the later stack bus transactions: DMA can finish during `RET`, so a
snapshot containing `ffff` does not imply that `RET` actually read `ffff`.

`raw_stack_bytes` and `raw_stack_after` read the same original-SP addresses
before and after the step, bypassing DMA arbitration; the line explicitly
labels this `raw_access=bypasses_dma raw_after_sp=original`. `dma_active`,
`dma_source`, `dma_bytes_copied`, `dma_start_delay` (CPU T-cycles), and
`double_speed` describe the pre-step DMA state. `rom_bank` is the selected
MBC5 switchable-window latch (all nine bits), not the bank containing every
reported PC; other mappers report `unavailable`. Numeric formatting follows
the instruction history, with DMA counters in decimal and bytes, addresses,
and bank values in hexadecimal. Snapshots do not advance hardware or change
emulated state. Their 64-entry ring is allocated only for `--trace-first-pc`,
stops collecting at the first hit, and writes only to stderr; an initial-PC
match or an unreached target emits no stack history.

See [the Shantae OAM-DMA investigation](shantae-dma-stack-analysis.md) for the
proven double-speed timing failure and its independent regression tests.

`--frames` continues to wait for completed emulator frames. The new mode requires
`--frame-output` and is mutually exclusive with `--frames`, `--frame-series`,
`--frame-on-ld-bb` and `--replay-sgb-trace`. It supports `--sgb-frame` for an SGB
border snapshot and ignores protocol pass/fail markers until capture.
`--max-cycles` remains a hard budget (default 100,000,000); if it is below the
target, the runner times out with no snapshot. Raise it for longer runs.
Input-script frame positions and SGB trace frame counters retain their existing
emulator-frame semantics; they do not advance on these virtual intervals.

For SGB investigations, capture an opt-in JOYP trace from a bounded real ROM
run, then replay it against the same legally obtained ROM:

```sh
./build/gbb_test_runner path/to/sgb.gb --model sgb \
  --max-cycles 5000000 --sgb-trace /tmp/sgb.trace
./build/gbb_test_runner path/to/sgb.gb --model sgb \
  --replay-sgb-trace /tmp/sgb.trace
```

The capture file is written on pass, failure, or timeout. Replay advances the
bus using the recorded cycle positions and stops at the first state or command
checkpoint mismatch; see [SGB trace and replay](sgb-traces.md).

The opt-in Mooneye comparison also offers `--suite interrupt-div` for ten pinned
CGB-applicable interrupt/DIV tests; the timer suite remains the default. See
[the reference workflow](gbvisualizer-reference.md) for external checkout pins,
commands and the limits of these conformance results. The authored
`gameboy_cgb_irq_div_probe_contract` checks first-vector DIV witnesses without
external tools. Set `GBB_TEST_RUNNER` and `GBB_SAMEBOY_CGB_CAPTURE` when running
`tests/cgb_irq_div_probe_tests.py` to enable the native 192-pair request-phase comparison. The probe CLI retains
the 96-pair default; `--request-phase-sweep` adds software IF one machine cycle
before and after the predicted hardware request.
`gameboy_cgb_irq_div_hardware_contract` checks the standalone hardware-witness
builder's deterministic headers and unchanged measurement path. Its optional
native test uses the same two environment variables to verify persistent raw
results, completion-only signatures and bounded missing-interrupt failure.
Build the eight flash-cartridge ROMs with
`python3 scripts/build_cgb_irq_div_hardware.py --output /external/hardware-roms`;
physical-hardware results remain a separate validation step.


Compare captures from two builds or devices without rerunning the ROM:

```sh
./build/gbb_sgb_trace_diff desktop.trace android.trace
```

The command reports the first differing write, checkpoint hash, diagnostic
counter, or decoded command packet.

### BullyGB external research suite

[BullyGB](https://github.com/Ashiepaws/BullyGB) v1.2 is included in the external
research harness. Its ROM stays outside this repository. The manifest pins
release source commit `a9dccf7fb2b68b7bb4c0cbe98248383087e70a13` and the official
`bully.gb` SHA-256 `05fb33ea49aaefe6a42f7619dbab85ec309a6e49ca961c649e8525a52a79a3e1`.
Download the upstream release into your external ROM root:

```sh
mkdir -p /external/test-roms/bullygb
curl --fail --location https://github.com/Ashiepaws/BullyGB/releases/download/v1.2/bully.gb \
  --output /external/test-roms/bullygb/bully.gb
python3 tests/run_external_suites.py --runner ./build/gbb_test_runner \
  --rom-root /external/test-roms --suite bullygb \
  --output /external/bullygb-report.md
```

Discovery verifies the ROM hash before execution. A missing or altered ROM inside
that directory is reported as failure. The existing external-suite CI job downloads
and verifies this separate asset, runs it alongside AGE/SameSuite, and publishes
the report and full logs. Ordinary offline tests need no external ROMs or network.

BullyGB is one sequential aggregate ROM: it stops on the first error, so six
model runs do not imply that every individual subtest executed. The selected
profiles are DMG, MGB, CGB0/CGB-C/CGB-E and AGB. SGB profiles are excluded until
their firmware/handoff prerequisites are explicitly configured. BullyGB itself
has broad CGB applicability rather than revision-specific expectations; these
profiles are observations, not independent hardware qualification.

`--protocol bullygb` is explicit and leaves automatic/Mooneye/serial protocols
unchanged. Upstream prints ASCII tile IDs starting at BG map address 9821,
wraps after 18 characters, enables the LCD and enters a `JR @` loop. It writes
SB without starting serial transfers, so ordinary serial success detection is
unsuitable. The adapter uses read-only VRAM access at the terminal loop and
requires the exact screen text `All tests OK!`; other nonempty results fail,
while incomplete/nonterminal output times out. Frame/cycle capture modes retain
their own completion rules. Protocol return codes are success 0, failure 1 and
cycle timeout 2.

Keep this suite in the **research** lane. Its initial DIV/VRAM/RAM and boot-register
checks depend on power-on/boot state; upstream explicitly marks some DIV model
expectations unconfirmed. The harness uses GBB's default startup profiles, without
proprietary boot images or modified upstream test code. A failure should be
investigated against the documented startup assumptions before changing core
behavior. No tests are bypassed and no expected failures are converted into passes.

Authored protocol fixtures cover exact success, wrapped error text, false-success
suffixes, incomplete screens, opt-in behavior and capture-mode isolation. They
are registered as `gameboy_bullygb_protocol`; external discovery/hash/protocol
contracts remain in `gameboy_external_suite_harness`.

The initial integration found six aggregate failures: DMG/MGB rejected cleared
WRAM; color profiles rejected the native CGB initial DIV. Follow-up fixes now
produce **five passes** (DMG, MGB, CGB-C, CGB-E, AGB) and **one retained CGB0
reference disagreement**. No tests or failure signatures are bypassed.

The corrections cover:

- Deterministic nonuniform WRAM at machine power-on, using a fixed PRNG seed
  without OS entropy. It approximates undefined memory, not measured SRAM-cell
  patterns. Cartridge battery RAM, HRAM and VRAM have separate contracts. Soft
  post-boot reset retains WRAM; cold boot recreates the same contents. Save-state
  format and replay determinism are preserved. Test guests must initialize their
  ISR counters rather than relying on zero-filled memory.
- Mode-dependent native CGB versus monochrome-compatibility boot divider and LCD
  phase, with AGB's four-cycle handoff offset. Native CGB0 retains its distinct
  divider; this is not changed to satisfy a later-CGB expectation.
- DMG/MGB post-boot logo tiles decoded from the loaded cartridge header, plus
  the two logo-map rows and existing trademark. No fixed cartridge logo artwork
  is added to the repository.
- FF74's fully writable native-CGB register, initially zero; it remains FF and
  read-only outside CGB mode.
- OAM DMA reads drive the contending CPU bus with the current source byte,
  including instruction fetches and stack reads. CGB cartridge and WRAM buses
  are independent, so ROM DMA permits WRAM stack access. DMG main-bus writes
  remain blocked. Raw/debug reads remain unrestricted, and DMA state uses the
  existing source/index fields without a save-format change.

Independent validation used pinned SameBoy 1.0.3
(`208ba4afabffab9edde416f2dbb8ae459e34adb8`) with privately supplied original
boot images strictly as execution-only black boxes. Unmodified BullyGB v1.2
passes in DMG/MGB/CGB-C/CGB-E/AGB there too. CGB0 reads DIV=21 at its test while
BullyGB expects 1F for every CGB revision, and fails identically in the reference.
Thus the remaining CGB0 row is an upstream applicability disagreement, not an
expected-failure override. Original images and generated ROMs are not committed.

A JP/NOP observer fixture retaining the BullyGB header brackets native DIV and
LCD-line edges via public execution callbacks and read-only IO. Native color
handoff counters are 20AC for CGB0, 1EA0 for CGB-C/E and 1EA4 for AGB; handoff
is at internal line 144, dot 168 (172 on AGB). The established monochrome
compatibility divider expectations remain covered separately by Mooneye. These
are boot/profile observations, not physical-hardware measurements or a general
claim of exact boot timing for arbitrary headers.

Private boot-image SHA-256 provenance:

| Profile | Boot image SHA-256 |
| --- | --- |
| DMG | `cf053eccb4ccafff9e67339d4e78e98dce7d1ed59be819d2a1ba2232c6fce1c7` |
| MGB | `a8cb5f4f1f16f2573ed2ecd8daedb9c5d1dd2c30a481f9b179b5d725d95eafe2` |
| CGB0 | `3a307a41689bee99a9a32ea021bf45136906c86b2e4f06c806738398e4f92e45` |
| CGB-C | `b4f2e416a35eef52cba161b159c7c8523a92594facb924b3ede0d722867c50c7` |
| CGB-E | `c56299bedd56debdbf36442238636bf5887a65c5173b33995682052353804da9` |
| AGB | `fe3cceb79930c4cb6c6f62f742c2562fd4c96b827584ef8ea89d49b387bd6860` |

Thirteen pinned Mooneye boot/DIV/IO/VBlank executions pass: DMG/MGB boot DIV,
CGB0/CGB-C/CGB-E boot DIV, CGB IO and VBlank/STAT, and AGB/AGB0 boot DIV. A further 32 OAM-DMA executions pass across the six
BullyGB profiles (basic, register read, restart, startup and finish timing; the
model-restricted source test is run only on DMG/MGB). These external tests are
built from Mooneye `31510e12eea6286d36eea060a6adde755e1067aa` with WLA-DX
`7ae11df1657dafab1f5fa662889d005709fec15e`. All 16 selected firmware, CPU,
timer, PPU, model, save-state and integration CTest contracts pass. Authored
native contracts cover nonuniform/reproducible WRAM, reset retention, color boot
mode phases, header-logo expansion, FF74 access and DMA-driven instruction/stack
cycles. The existing DMA/save-state and firmware lifecycle tests also apply.

To run the optional pinned BullyGB aggregate regression locally:

```sh
GBB_TEST_RUNNER=./build/gbb_test_runner \
GBB_BULLYGB_ROM_DIR=/external/test-roms/bullygb \
  python3 tests/external_suite_tests.py
```

The test requires the exact pinned ROM hash and checks all five supported
aggregate passes while retaining the independently observed CGB0 mismatch.

### SameSuite APU applicability

`tests/samesuite_models.py` is shared by the external report and legacy direct
CTest registration. Model selection follows explicit filename suffixes:

| Filename suffix | Runnable profiles | Unsupported revisions retained |
| --- | --- | --- |
| `-cgb0` | CGB0 | None |
| `-cgb0B` | CGB0 | CGB-B |
| `-cgb0BC` | CGB0, CGB-C | CGB-B |
| `-cgbBCE` | CGB-C, CGB-E | CGB-B |
| `-cgbDE` | CGB-E | CGB-D |
| `-cgbB` | None | CGB-B |
| `-A` | AGB | None |

Untagged digital APU tests retain the CGB-E research profile. The two
`div_write_trigger`/`div_write_trigger_10` ROMs retain DMG. Unknown explicit
suffixes are reported unsupported. No unsupported revision is substituted with
a nearby CGB revision. The AGB entry uses the existing AGB startup profile and
remains research evidence, not a claim of validated AGB APU silicon behavior.
The revision distinction is documented in
[upstream's CH3 length test](https://github.com/LIJI32/SameSuite/blob/f15645fb049a47ea235f6d2c9a033e72d8087901/apu/channel_3/channel_3_extra_length_clocking-cgb0.asm).

For mixed labels the supported members run and each unsupported member gets
its own visible row. In the report those rows have status `unsupported`; direct
CTest emits an explicit unsupported message and records the test as skipped.
Direct test names include the model to keep multi-profile registrations unique.
SGB informational cases and non-APU research selection are unchanged.

On pinned bundle v7.0, the revised report contains 36 passes, 40 failures, six
unsupported revisions and two informational entries. Direct registration selects
70 runnable APU cases and six explicit skips. The four fewer failures follow
selection changes; all 76 common case IDs/profiles retain their previous result.
No emulator/APU timing changed in this step.

`tests/external_suite_tests.py` covers exact/mixed revision selection, unknown
suffixes, unsupported non-execution, and the CMake resolver CLI. A small authored
CMake fixture compares actual direct registrations with report discovery,
including paths with spaces, multi-profile names and explicit skip properties.
It requires no upstream ROM, compiler, build or network access.

The opt-in [SameSuite alignment comparison](accuracy.md) records four pinned
CH1/CH2 tests against SameBoy, with full repeated PCM curves and explicit result
protocols. `gameboy_samesuite_alignment_reference_contract` runs offline. Set
`GBB_TEST_RUNNER`, `GBB_SAMEBOY_CGB_CAPTURE` and `GBB_SAMESUITE_ROM_DIR` when running
`tests/samesuite_alignment_reference_tests.py` to enable the paired native check.
The comparison returns 1 for any protocol/curve disagreement; all four tests now
pass and match after the NR52-relative pulse-clock correction. Tool-contract
success remains separate from hardware qualification.

`scripts/cgb_pulse_phase_probe.py` builds independently authored, asset-free
CGB-E controls for both pulse channels, with separate 0..3-NOP NR52-enable and
trigger phases at normal and double CPU speed. Each of the 64 cases records
32 PCM12/NR52 pairs in bounded WRAM, with timer/interrupts/LCD disabled during
measurement. Use the same `--runner`, `--sameboy-capture`, and fresh `--output`
arguments as the alignment comparison; optional `--case ch1-enable-0-trigger-1`
restricts the run to one scenario at both speeds. Reports preserve complete
curves, per-case ROM and executable hashes, commands, repeated captures and
sample differences. Exit 1 means disagreement or invalid/non-repeatable data;
exit 2 means a capture or input error. Reports always set `qualification=false`.
`gameboy_cgb_pulse_phase_probe_contract` runs offline. Set `GBB_TEST_RUNNER` and
`GBB_SAMEBOY_CGB_CAPTURE` to run the complete native matrix through
`tests/cgb_pulse_phase_probe_tests.py`; no external test ROM is required.

For frequency-write timing, run `scripts/samesuite_frequency_reference.py` with
the same arguments as the alignment comparison. Its separate pin checks the
v7.0 CGB-D/E frequency-write ROM before execution; the selected model is CGB-E.
The report contains all 16 packed immediate/late PCM observations from normal
and double CPU speed, repeated protocols and frozen completion checks.
`scripts/cgb_pulse_frequency_probe.py` needs only `--runner`,
`--sameboy-capture` and a fresh `--output` directory. It authors 12 controls
covering both pulse channels and CPU speeds, with high-register, low-register
and unchanged-frequency writes. Each control records 64 immediate/late PCM12
pairs plus NR52. Optional `--case ch1-high` restricts it to one scenario at both
speeds. Both tools return 1 for a disagreement, 2 for input/capture errors, and
keep `qualification=false` even when the comparison succeeds.

`gameboy_cgb_pulse_frequency_probe_contract` and
`gameboy_samesuite_frequency_reference_contract` run offline. Set
`GBB_TEST_RUNNER` and `GBB_SAMEBOY_CGB_CAPTURE` to enable native measurements in
`tests/cgb_pulse_frequency_probe_tests.py`; additionally set
`GBB_SAMESUITE_ROM_DIR` for `tests/samesuite_frequency_reference_tests.py`.

Add `--lifecycle` to `scripts/cgb_pulse_frequency_probe.py` to select the separate
48-case active-retrigger/speed-switch matrix. It covers both pulse channels,
both initial CPU speeds, high/low/unchanged-frequency writes and an equal-cost
no-write control, with retrigger-only, switch-only and switch-then-retrigger
sequences. `--case ch1-observe-retrigger` selects one no-write scenario at both
initial speeds; lifecycle cases require `--lifecycle`. KEY1 witnesses confirm
the initial speed and each observation's final speed with the prepare bit
cleared. The report's `double_speed` is the initial speed; lifecycle captures
also record `initial_double_speed` and `final_double_speed`. Delay timestamps
use the last retrigger or speed-switch completion as their stated anchor.
Switch scenarios restore the initial speed before resetting NR52/DIV for the
next observation. The original default matrix, schema and ROM bytes remain
unchanged. The lifecycle schema is `gbb-cgb-pulse-frequency-lifecycle-probe-v1`;
exit 1 retains any reference disagreement, including in the no-write controls.
The optional native test covers both matrices and does not require known
lifecycle differences to persist. Offline tests verify original ROM hashes,
speed witnesses, completion/guard checks and bounded scenario selection.

`scripts/cgb_pulse_retrigger_probe.py` isolates retrigger timing without STOP or
frequency writes after the measured operation. Use `--runner`,
`--sameboy-capture` and a fresh `--output` directory; optional
`--case ch1-retrigger-gap-1` selects one scenario at both CPU speeds. The 48-case
matrix covers both pulse channels, active retriggers and equal-cost no-retrigger
controls, at gaps of 0/1/2/3/8/32 NOPs. Each case independently sweeps 64 PCM read
delays, checking NR52, KEY1, completion and a WRAM guard. Initial double speed
is selected before APU measurement begins. Reports retain complete curves,
ROM/executable hashes, repeated observations and differing indices, with
`qualification=false` and the same 0/1/2 success/disagreement/error convention.
`gameboy_cgb_pulse_retrigger_probe_contract` runs offline; set
`GBB_TEST_RUNNER` and `GBB_SAMEBOY_CGB_CAPTURE` for the complete native matrix
in `tests/cgb_pulse_retrigger_probe_tests.py`. The lifecycle native contract now
requires parity for all 48 cases, including the speed switches.

`scripts/cgb_stop_apu_probe.py` isolates STOP speed switches with active,
configured-but-untriggered and powered-off APU controls. Use `--runner`,
`--sameboy-capture`, and a fresh `--output`; optional
`--case ch1-active-switch-period-28` selects one scenario at both initial CPU
speeds. The 48-case matrix covers both channels, periods of 16/28 APU clocks,
switches and single-NOP controls. Each case records 128 PCM12/NR52/KEY1/DIV/
TIMA/IF observations, preserving separate PCM and timer comparisons. NR52,
initial/final KEY1, completion and the adjacent WRAM guard validate each log.
The output retains all differences and returns 1 for disagreement, with
`qualification=false`; capture errors return 2. All 48 cases now match PCM and timer observations with the staged CGB-E
transition; the native contract requires full parity. Timing-window boundary
comparisons remain separate.

These longer STOP controls and the frequency lifecycle matrix use bounded
limits of 100 million GBB CPU T-cycles
and 200 million reference 8-MHz clocks per capture, with the existing 30-second
child timeout. The shared harness's default limits remain 20/40 million;
offline tests verify defaults, overrides and invalid-budget rejection before
output creation. `gameboy_cgb_stop_apu_probe_contract` runs offline. Set
`GBB_TEST_RUNNER` and `GBB_SAMEBOY_CGB_CAPTURE` for the full native matrix in
`tests/cgb_stop_apu_probe_tests.py`. Native checks require repeatability, channel-pair agreement and PCM/timer
parity for every case.

`scripts/cgb_stop_timing_probe.py` measures a single operation and resumed
instruction, without relying on frame callbacks or seeing only a PC advance
before a pause ends. Rebuild the pinned reference adapter to add
`--pc-clock-window`, then use `--runner`, `--sameboy-capture`, and a fresh
`--output`. Its 20 cases combine both CPU speeds, switch/NOP operations and
active/armed/powered-off APU states, with additional `INC B` and two-cycle
`LD B,11` resume controls. GBB uses its existing first-PC witnesses and explicit
Mooneye completion; reference observations use bounded public-API callbacks.
Two takes at each boundary verify repeatability. The report includes separate
CPU T-cycle and 8-MHz clock fields, callback/register witnesses, commands and
executable/ROM hashes. Reference calibration includes the first resume callback,
completion of that instruction, and the subsequent steady NOP boundary.

Version 2 of the timing report uses `CPU_FIRST_PC normalized_8mhz`, accumulated
by the bus at its actual speed, including transition idle steps. This field is
also emitted on recent/history records at instruction exit; `cycle` retains
its existing CPU T-cycle meaning. State version 44 retains the bus clock epoch
and in-flight transition. Older states resume their saved atomic STOP behavior
and begin a new normalized clock epoch at zero. The historical version-1 report
used an atomic-STOP speed partition and remains recorded as baseline evidence.
Timing or register disagreements return 1, malformed/incomplete witnesses return 2, and successful
comparisons retain `qualification=false`. `gameboy_cgb_stop_timing_probe_contract`
runs offline. Set `GBB_TEST_RUNNER` and `GBB_SAMEBOY_CGB_CAPTURE` (the rebuilt
adapter) to run the full native matrix and invalid-argument/timeout checks in
`tests/cgb_stop_timing_probe_tests.py`. Existing adapter capture/protocol modes
also have native regression coverage.
