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

## Performance and load testing

Normal native test builds include a deterministic emulation load test and a
focused FPS-metrics unit test. The load test runs a synthetic ROM for 600
frames, exercises CPU, PPU, timer, APU, input, and framebuffer paths, and
prints structured `performance_metric` records for the emulated FPS, cycles,
audio samples, and FPS measurement windows:

```sh
cmake -S . -B build-performance -DCMAKE_BUILD_TYPE=Release
cmake --build build-performance --target \
  gameboy_frame_rate_metrics_tests gameboy_emulation_performance_tests
  gameboy_sgb_performance_tests
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

## Fuzzing

Parser/protocol fuzzing is opt-in and requires a Clang toolchain with
libFuzzer. Build it alongside the native sanitizers, then provide a corpus
directory (libFuzzer will create and extend it):

```sh
cmake -S . -B build-fuzz -G Ninja \
  -DCMAKE_CXX_COMPILER=clang++ \
  -DGAMEBOY_BUILD_SDL=OFF \
  -DGAMEBOY_BUILD_FUZZERS=ON
cmake --build build-fuzz --target gameboy_parser_fuzzers
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
bash tests/fuzz/promote_corpus.sh fuzz-corpus-run tests/fuzz/corpus --approve
```

The promotion command uses libFuzzer's merge mode in a temporary directory and
never changes the reviewed corpus during the dry run.

The target feeds bounded inputs through settings, trace, save-state,
link-packet, and SGB parsing boundaries. Fuzzing is deliberately separate from
the normal CTest suite so release and cross-platform builds remain dependency
free.

## Sanitizers and frontend smoke tests

The CI ThreadSanitizer job also builds the SDL frontend with SDL enabled,
runs the display-independent contract tests, and performs a bounded dashboard
window smoke under Xvfb. Longer interactive sessions remain part of the normal
desktop build because they require a display server and user input.

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

For a local dashboard smoke, use an Xvfb display and stop after a short bounded
run:

```sh
SDL_AUDIODRIVER=dummy SDL_VIDEODRIVER=x11 WAYLAND_DISPLAY= \
  SDL_RENDER_DRIVER=software \
  TSAN_OPTIONS=halt_on_error=1 \
  timeout --signal=INT --kill-after=5s 12s \
  xvfb-run -a ./build-tsan-sdl/gbb
```

The Web frontend has a browser-level smoke that loads the real WebAssembly
bundle, changes the persisted display/audio settings, reloads the page, and
opens a small synthetic ROM. CI runs it with headless Chromium. With Node.js
and Playwright available locally:

```sh
python3 -m http.server 8765 --directory build-web/web
npx --yes @playwright/test@1.52.0 install chromium
npx --yes @playwright/test@1.52.0 test \
  --config=tests/web/playwright.config.mjs tests/web/frontend.spec.mjs
```

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
`--model dmg0`, `dmg`, `mgb`, `sgb`, `sgb2`, `cgb0`, `cgb-c`, or `cgb-e`.
The historical `cgb` spelling remains accepted as the late CGB-E profile.
For boot-path diagnostics, add `--diagnostic-boot`; this runs GBB's original
diagnostic boot ROM, records a handoff marker in HRAM, validates the CPU
handoff state, and then continues at the cartridge entry point. Normal
emulator construction continues to use the existing post-boot path.

The APU passes all 12 upstream Blargg `dmg_sound` tests and all 12 `cgb_sound`
tests, including model-specific power behavior, active wave-RAM access, and the
original DMG hardware's channel 3 retrigger corruption.
The current headless CI accuracy gate passes all 75 Mooneye acceptance ROMs,
all 6 applicable CGB misc ROMs, all 28 emulator-only mapper ROMs, 38 curated
Blargg ROMs, and 21 exact Acid2/Scribbltests/Mealybug/Gambatte framebuffer
comparisons. The separate [hardware-model matrix workflow](https://github.com/DanielSeim/go-bigger-boy/actions/workflows/hardware-model-matrix.yml)
evaluates additional GBMicrotest and Mooneye-wilbertpol cases. The desktop
workflow also publishes a research report for AGE and SameSuite: it discovers
AGE screenshot references, applies its DMG-compatibility color rules, maps
SameSuite diagnostics to documented hardware profiles, and keeps exploratory
failures outside the release gate (see the [accuracy report](accuracy.md)).
Each run publishes the full Markdown reports as downloadable artifacts.

To register the pinned v7.0 bundle locally, download and extract
`c-sp/game-boy-test-roms`, then set its root as the opt-in cache path. The
test ROMs are deliberately not bundled or downloaded by the build:

```sh
cmake -S . -B build-conformance \
  -DGAMEBOY_TEST_ROM_DIR=/path/to/game-boy-test-roms-v7.0
cmake --build build-conformance
ctest --test-dir build-conformance -L conformance --output-on-failure
```

To run the metadata-driven AGE and SameSuite research harness locally, add
`-DGAMEBOY_ENABLE_EXTERNAL_SUITES=ON` to the configure command and run:

```sh
ctest --test-dir build-conformance -L external-suites --output-on-failure
```

The report is written to `external-suite-report.md`. Research failures are
reported for review but do not fail unless a suite entry is explicitly
promoted to the `required` gate.

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

The headless runner can also capture a deterministic framebuffer without SDL:

```sh
./build-conformance/gbb_test_runner test.gb \
  --model dmg --frames 60 --frame-output capture.ppm
```

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

Compare captures from two builds or devices without rerunning the ROM:

```sh
./build/gbb_sgb_trace_diff desktop.trace android.trace
```

The command reports the first differing write, checkpoint hash, diagnostic
counter, or decoded command packet.
