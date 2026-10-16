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

Local validation of the pinned unmodified v1.2 ROM found six aggregate failures:
DMG/MGB report `Uninitialized RAM not randomized`; CGB0/CGB-C/CGB-E/AGB report
`Invalid initial   DIV`. Every run reached an explicit terminal error, rather
than timing out. These are baseline observations with the default startup
profiles, not exemptions or complete subtest coverage. Emulator startup behavior
was left unchanged by the suite integration.
