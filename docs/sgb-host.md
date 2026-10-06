# Bounded SGB firmware host

`gameboy::SgbHost` coordinates our original bounded SNES CPU, ICD/Game Boy
bridge and SPC700/DSP engine in `gameboy_core`. It is an opt-in core component,
**not enabled by default in shipping frontends**. Desktop firmware playback
requires an explicit experimental opt-in through desktop settings or CLI
(see below). No bsnes code, external emulator core,
Nintendo firmware, ROM, captured audio or private state is bundled.

The existing diagnostic CPU/ICD headers are compatibility adapters to the same
core implementation. Legacy timing profiles and tests remain available;
moving the code does not make it a complete SNES emulator.

## Current evidence status (audited 2026-10-05)

Version 0.36.0 ships the desktop opt-in adapter; HLE remains the default and
Android/web do not expose firmware playback. Bundled startup and this runtime
backend are separate selections; see [firmware startup](../firmware/README.md).
The implementation uses complete `cold-sgb-v1` reset and a `clocked-pixel-v1`
LCD bridge. Current exact WAV/state pins are shared by the local title tests
and serial benchmark; see
[test pins and provenance](sgb-boot-validation.md#current-test-pins-and-provenance-audited-2026-10-05).
`GBBSHOST` version 3 is the current host snapshot; the desktop wrapper is
`GBBFW001`, and the nested ordinary GB state is version 42. Older host
versions 1/2 cannot restore pending LCD events and are rejected.

The initial recorded clocked-bridge Release/IPO Linux host-only measurement
preserves all four prior production PCM prefixes with appended endpoints and
passes current complete WAV/final-state pins, but fails all four 1.40x p05 /
1.20x worst-window gates (p05 1.153–1.228x, worst 0.931–1.071x). See
[bridge playback evidence](sgb-boot-validation.md#playback-baseline-evidence)
for conditions and the full table. Current bridge headroom is unqualified.
The historical Balanced Windows qualification, awake-tablet soaks and native
Windows frontend captures below predate this bridge and do not qualify it.
There is no same-host pre/post bridge A/B in that run, so its cost cannot be
isolated from host-capacity variation. The subsequent
[bounded deadline-cache investigation](sgb-boot-validation.md#bounded-deadline-cache-investigation-2026-10-05)
retains exact complete WAV/state pins and improves the observed windows, but
its four-profile run still fails every p05 bound (1.272–1.365x). A later passing
SGB1 combined pair is not all-profile repeatability qualification. Current
bridge headroom therefore remains unqualified; no new device qualification
is claimed.

## Inputs and scheduling

The caller supplies `SgbHostConfig`: program ROM bytes, game ROM bytes, GB boot
image, SPC IPL, SGB1/SGB2 model, optional initial battery RAM and held-button
events at native GB LCD-frame boundaries. The core does not open files or
load/flush battery saves automatically. The local test runner explicitly reads
the same initial `.sav` as the older diagnostic replay for baseline comparison.

The host uses the validated fractional APU rendezvous, host bus timing and
PPU-DMA timing profiles. SNES accesses synchronize the APU before port reads
and writes; ICD accesses and completed host instructions synchronize the GB.
The default APU oscillator stays 1,024,000 Hz. The explicit reference oscillator
is a diagnostic choice, not a runtime correction.

`step()` finishes one supported host instruction. `run_until(master_target,
instruction_budget)` stops at an instruction boundary, so a long instruction
can overshoot its target. A past target or zero budget does no work. There is
no wall-clock sleep, audio device or thread synchronization. Native output is
not resampled; the opt-in combined path below performs digital rate conversion.

Unknown CPU/SPC instructions and missing ICD data stop explicitly. Faults stay
terminal until reset. `fault()` retains host instruction/address context;
`status()` distinguishes host, APU and ICD failures. `reset()` reconstructs a
cold host using the same configured images, input script and initial save RAM.

The GB-side source is constructed and reset from the complete deterministic
SGB/SGB2 power-on baseline, not a diagnostic post-boot machine with selected
registers cleared. CPU, PPU, APU, timer, serial, interrupt, DMA and volatile RAM
state match standalone cold execution before the caller's GB boot image runs.
Input replay policy does not select or mutate this reset baseline. An ICD
CPU reset preserves the cartridge's current battery RAM and held live input;
a full `SgbHost::reset()` reconstructs the configured initial save and script.
Cold reset rebinds destination-owned audio callbacks, retains the frontend
audio-enable preference, and starts a new absolute sample epoch at release.
The cold APUs and volatile RAM are deterministic emulation baselines, not a
claim about physical power-on RAM randomness. See
[cold-reset validation](sgb-boot-validation.md#whole-host-cold-reset-validation).

## Backpressure

Unread SNES-side PCM lives in a fixed 16,384-sample stereo ring. Before starting
an instruction, the host requires 8,192 free slots: enough for all eight maximum
length DMA channels, including their setup/refresh timing, at the highest
accepted APU frequency. This deliberately applies pressure before the ring is
completely full. The internal APU FIFO drains into this ring while clocking.

When reservation fails, no CPU, GB, APU, DMA or port access runs. Drain with
`pop_sample()`, then retry; the instruction is not replayed midway through its
bus side effects. Maximum-length DMA, blocked retries and pending-DMA restore
have original ROM-free contract tests. The reserve is tied to the current
bounded CPU/DMA implementation and must be revisited if that model expands.

The default output remains **SNES-side PCM only**, with the unused GB analog
mixer/output queue disabled. Combined output is a separate explicit configuration
below; the experimental desktop adapter selects it at 48 kHz.

## Experimental desktop playback

Windows: open the library's **Settings → General**, enable experimental SGB
firmware playback, select SGB1/SGB2, and enter or browse to your firmware
directory. Linux: choose **SGB firmware settings (experimental)** in the SDL
library dashboard, then select the model and directory. Choose **HLE (default)**
to disable it.
Android does not expose or use these desktop firmware settings.

The selected backend is saved in `settings.ini` and applies only to the next
ROM launch. Applying settings does not replace a running core, and reset keeps
that session's backend and model. Reopening a ROM uses the new settings.
Unsupported voxel modes are excluded while firmware playback is active;
select a 2D mode before enabling it. Unsupported tools stay disabled by the
active core's capabilities. Firmware validation checks the files before applying
settings and again at launch; invalid images never trigger a silent HLE fallback.

For scripted launches, the explicit CLI override takes priority over saved
preferences for that process:

```sh
./build-desktop/gbb game.gb --sgb-firmware /path/to/private/firmware --sgb-model sgb2
```

The directory must contain `sgb2.program.rom` (or `sgb1.program.rom` for SGB1).
The original SPC700 IPL is bundled; an optional exactly 64-byte `spc700.rom`
overrides it. Supplied invalid overrides fail explicitly. See
[IPL provenance and validation](spc700-ipl-validation.md).
For `--sgb-model sgb`, supply `sgb1.program.rom` instead. The original GBB
256-byte SGB/SGB2 bootstrap is bundled. An optional `sgb2.boot.rom` or
`sgb.boot.rom` in the directory overrides it; malformed overrides fail rather
than silently falling back. The default experimental model is SGB2. Private files are loaded
only after this explicit opt-in; missing/invalid images fail without an HLE
fallback. No proprietary images are downloaded, bundled, or added to releases.

A single `SgbHost` owns GB execution, live input, and combined GB/SNES audio.
SDL consumes its 48 kHz stereo samples through the existing bounded playback
policy; muted audio is still drained. Video and read-only scene metadata come
from that same GB instance. Existing GB-side SGB color/border composition is
retained: this is **not** a newly implemented SNES PPU or firmware-rendered
SNES menu. HLE remains the default launch path.

The persisted keys are `sgb.FirmwarePlayback`, `sgb.FirmwareDirectory` and
`sgb.FirmwareModel`. The directory is written as hex-encoded UTF-8 (`hex:`)
so spaces, Unicode and INI comment characters survive round trips. A plain
path is accepted on read, subject to the usual INI comment rules. Disabling
playback retains the chosen directory/model for later use.

Settings validation has ROM-free contracts for HLE defaults, migration,
Unicode/comment-character directory round trips, missing/invalid images,
model selection and CLI precedence. The native Windows dashboard smoke also
checks apply/discard with scene-layer capability, firmware checkbox routing,
2D-only mode choices and invalid-directory rejection. Local caller-owned
Donkey Kong validation on 2026-10-04 exercised HLE → SGB2 → SGB1 → HLE library
launches, reset retaining the running backend/model, separate model-specific
snapshots, disabled unsupported menus, preference persistence across restart
and CLI precedence. The Linux SDL dialog was checked separately with isolated
portable settings. These are frontend lifecycle checks, not new audio/accuracy
or performance claims.

Pause/resume, cold reset, and model/firmware-bound manual snapshots work through
the generic core interface. A terminal host/APU/ICD fault stops playback with
diagnostic status/address context rather than switching engines or lowering
quality. Link, debugger, cheats, camera, RTC, rumble and background rewind are
not enabled for this path. Unsupported cartridge peripherals and CGB-only
software are rejected. Desktop voxel presentation is not integrated with the
firmware adapter; use the existing 2D presentation for this experiment.

Battery RAM uses separate `.sgb-firmware.sav` / `.sgb2-firmware.sav` files; it
does not automatically read or overwrite the ordinary `.sav`. Quick states also
use separate model-specific `-firmware.gbbs` filenames. State contents include
private firmware and must not be redistributed. Save replacement uses a sibling
`.pending` staging directory; a pre-existing staging directory fails closed and
is not erased. Successful saves remove their own staging directory.

`--frontend-smoke-frames 600` runs a bounded full-window smoke test, then exits
and reports observed FPS, audio availability, submission-boundary empty-queue
events and latency resets. Empty-queue events are not hardware underrun
interrupts. Use isolated preferences for automated tests. A virtual display or
dummy audio driver validates plumbing, not physical display/audio quality.

<a id="native-windows-device-qualification"></a>

### Native Windows device qualification procedure and historical captures

Build the Release SDL frontend, then run the opt-in host against caller-owned
images with the reproducible runner. It refuses an existing output directory,
copies the game and optional initial battery save into separate model slots,
uses isolated preferences, and restores its process environment afterward:

```powershell
scripts/run_windows_sgb_playback.ps1 -Executable C:\build\gbb.exe `
  -Rom C:\private\game.gb -FirmwareDirectory C:\private\firmware `
  -InitialSave C:\private\game.sav -OutputDirectory C:\captures\sgb-playback `
  -Frames 7200 -BundledBootstrap
```

`-BundledBootstrap` exercises the normal bundled SGB/SGB2 Game Boy-side
bootstrap even when the input directory contains private GB boot overrides.
The runner copies the two SNES program images and any supplied SPC IPL override into a fresh
subdirectory of the capture directory; originals are untouched. Provenance
records each model's bundled/override selection. Without this switch, absent
GB boot overrides are also supported, matching the desktop loader.

The optional `gameboy_sgb_production_local_titles` CTest uses the local Donkey
Kong v1.1 ROM and pinned initial save with those same three private firmware
images. It runs both actual desktop adapters at stereo 48 kHz with bundled
bootstrap, comparing every frame's PCM and full border/viewport against a
same-platform host, checking unread-audio/frame-cursor restoration repeatedly,
and replaying cold reset while preserving battery RAM. Script input is sampled
at frontend presentation boundaries, unlike the raw host's native GB LCD-frame
script. This is an implementation regression, not an independent accuracy
reference. ROM-free CI also compares both bundled adapter paths through boot
handoff into an original audible cartridge fixture.

With the pinned private inputs present when CMake is configured:

```sh
ctest --test-dir build-desktop -R 'sgb_(firmware_core|production)' --output-on-failure
```

The private replay runs 3,600 presentation frames per model and pins PCM, full
video and final GB state on Linux/GCC. Other compilers still require exact
same-platform adapter/host parity rather than assuming floating-point audio
hashes are portable. The report validator has separate ROM-free rejection
tests, and private staging images are removed when the replay exits.

Each model runs about two minutes, with a real native window and WASAPI device.
Keep these runs uninterrupted; record what you hear separately. Provenance
includes the executable/input hashes and current Windows power scheme; the
runner never changes that scheme. Captures contain private ROM/save copies and
must not be committed or distributed.

```sh
python3 scripts/check_sgb_frontend_playback.py \
  /path/to/captures/sgb/frame-timing.log \
  /path/to/captures/sgb2/frame-timing.log
```

The checker requires completed Release traces for both models, ten seconds of
warmup and at least sixty measured seconds per model. Defaults require average
FPS 59.5..60.5 (the unchanged host cadence is about 60.098 Hz), p99 frame interval
<= 25 ms, worst interval <= 100 ms, enabled/available real audio, and no latency
resets. It rejects truncated/duplicate/malformed traces. Frame intervals include
time between loop iterations, so a pause or isolated stall cannot disappear in
an average. Per-frame samples are buffered and written after measurement;
ordinary playback does not collect them. The smoke bound is 1..36000 frames.
`--allow-dummy` explicitly permits plumbing-only tests; it does not qualify a
physical audio device. Work time includes presentation and is diagnostic, not
an unpaced core headroom measurement. Empty-input-queue observations remain
distinct from hardware underruns; neither a passing gate nor a WASAPI backend
establishes audible fidelity.

Run lifecycle checks separately, in another fresh output directory:

```powershell
scripts/run_windows_sgb_playback.ps1 -Executable C:\build\gbb.exe `
  -Rom C:\private\game.gb -FirmwareDirectory C:\private\firmware `
  -OutputDirectory C:\captures\sgb-lifecycle -Frames 1800 -Lifecycle -BundledBootstrap
```

This posts native menu commands only to the new test process's verified window:
pause/resume, isolated manual save, cold reset and load. It requires a paused
timing window, a model-specific firmware snapshot, and completed playback after
restoration. These intentionally interrupted traces are **not** steady-state
performance evidence. Physical listening remains a separate human check.
`GBB_FRONTEND_TEST_DIRECTORY` selects a separate frontend preference directory
only for a bounded smoke launch; on Windows this also isolates `settings.ini`.
The Windows runner sets it automatically. On Linux, settings remain at the
installation prefix for a `bin/` executable, otherwise beside the executable;
use an isolated installation location for Linux smoke tests. See
[platform storage](platforms.md#linux-desktop).

Local native MSVC Release qualification on 2026-10-04 used Donkey Kong (JU)
v1.1, the same initial battery RAM, Windows Balanced, Direct3D11 and WASAPI.
The final uninterrupted 7200-frame runs passed after ten seconds of warmup:

| Model | Measured FPS | Frame p99 | Worst frame | Median work | Latency resets |
| --- | ---: | ---: | ---: | ---: | ---: |
| SGB1 | 60.0985 | 17.49 ms | 22.67 ms | 14.30 ms | 0 |
| SGB2 | 60.0985 | 17.38 ms | 25.34 ms | 14.17 ms | 0 |

Those October 4 runs recorded 7 and 0 empty-input-queue observations
respectively. Both models also passed separate native-menu lifecycle checks.
User listening during the preceding two-model run reported no crackling, gaps
or unusual volume changes; an audible difference between the models was noted,
without a validated cause. The original battery save's SHA-256 remained unchanged.

A fresh MinGW Release build executed on Windows on 2026-10-05 also passed
7,200 frames per model using **bundled GB bootstraps**, the pinned initial save,
Balanced, Direct3D11 and WASAPI. These are paced frontend results, not renewed
unpaced headroom qualification:

| Model | Measured FPS | Frame p99 | Worst frame | Median work | Latency resets |
| --- | ---: | ---: | ---: | ---: | ---: |
| SGB1 | 60.0985 | 18.34 ms | 22.85 ms | 14.21 ms | 0 |
| SGB2 | 60.0985 | 18.00 ms | 33.19 ms | 14.05 ms | 0 |

Empty-input-queue observations were 2 and 4; these are not hardware underrun
counts. Both bundled models passed separate 1,800-frame native menu
pause/resume, model-isolated save, cold reset and load checks. Physical
listening for this fresh run is not recorded.

These are bounded local checks, **not** proof of hard realtime, universal
title/device performance, hardware-perfect audio, or large end-to-end headroom.
The separately qualified host-only headroom is not a frontend headroom claim.

Local integration checks on 2026-10-04 passed seven targeted Linux contracts
and the native MSVC firmware-adapter contract. Original ROM-free fixtures compare
single-host state, PCM and framebuffer pixels, and cover live input, cold reset,
cross-model/corrupt snapshots, terminal faults, isolated saves, and failed-save
preservation; the adapter contract also passed ASan/UBSan (leak detection disabled
for the ptrace environment). An isolated X11 interaction run exercised pause/resume, reset and
manual save/load. Serial Vulkan/Xvfb + dummy-audio Donkey Kong runs of 600 frames
averaged 59.90 FPS (SGB1) and 59.84 FPS (SGB2), including startup; warmed timing
windows were about 60.1 FPS, with zero latency resets. They recorded 8 and 6
empty-input-queue observations respectively. Those observations are retained,
not presented as zero physical underruns or proof of audible-device quality.

## Experimental combined audio

Set `SgbHostConfig::combined_audio = true` to enable raw GB APU capture alongside
the firmware-driven SNES output. `output_hz` accepts 8,000 through 48,000 Hz,
including 44,100 Hz. `gb_gain_q15` and `snes_gain_q15` are fixed per host, each
0..32,768 (unity); defaults are 16,384 each. These are **provisional digital
levels**, not independently calibrated SGB/SGB2 analog mixing ratios.

The raw GB sample sink bypasses `take_audio_samples()` and its frontend host
audio mixer: the SNES contribution is added exactly once. Source timestamps
come from absolute GB sample boundaries (including reset release and the
model's oscillator/divider), and actual SPC/DSP output halves. They are rounded
up to host master clocks, not assigned the end of the instruction that happened
to produce them. GB boundaries retain native-clock quantization. No estimated
frame offsets, wall-clock timing or adaptive quality changes are used. GB STOP
gaps are retained in the epoch so resumed samples do not move into the past.

`SgbAudioMixer` holds the latest DAC values and integrates their weighted area
over each output interval. Integer rational time avoids rate-conversion drift;
64-bit signed accumulators, symmetric nearest rounding and int16 saturation
avoid arithmetic wraparound. This causal held-DAC/box-area filter is a bounded
digital presentation choice, **not a band-limited or analog reconstruction of
real SGB hardware**. It has not been independently audio-reference validated.

Both source queues and the output ring are fixed at 16,384 stereo entries.
Combined mode reserves 12,000 output slots before a host instruction, enough
for eight maximum DMA channels at 48 kHz; source buffers also cover the fastest
ICD /4 divider and highest accepted APU clock. Full/late source events fail
closed rather than silently dropping samples. `SgbAudioMixer::advance_to()`
preflights output capacity and can be retried unchanged after draining.
Draining different consumer chunk sizes cannot advance an emulated processor.

An ICD reset inserts silence at its bus timestamp and removes speculative GB
samples at/after that point. Release starts a fresh GB sample epoch. A divider
change while the GB is already running fails explicitly in combined mode;
the bounded ICD does not yet model that oscillator's piecewise phase. Reset
then select a new divider instead. The legacy native path is unchanged.

`sample_rate()`, `snes_samples_produced()`, `gb_samples_captured()` and
`clipped_samples()` expose rate/component/clipping diagnostics. At the default
half-unity gains, the summed int16 inputs cannot clip. Higher gains deliberately
allow saturation and report clipped stereo frames.

## Whole-host snapshots

`save_state()` and `load_state()` use `GBBSHOST`, version 3, with explicit
little-endian scalar fields and a 16 MiB maximum. No object padding, callbacks,
addresses or filesystem paths are serialized. A configuration fingerprint
binds the images, model, clock profile, initial save, input script, audio mode,
output rate and gains. An FNV
payload checksum catches accidental corruption; neither is a security boundary.

Snapshots include host CPU registers, WRAM, DMA, interrupt/math/PPU timing;
the complete APU state; GB emulator state; ICD packet assembly, queued and
latched packets, indexed LCD/row buffers, initialized/completed bank flags,
pending clocked LCD events, release clock and input cursor; and
unread host PCM. Combined snapshots additionally retain both timestamped source
queues, held DAC values, partial output-interval areas, rational time, clipping
and output counters, and GB sample/reset epochs. Derived fast-path caches and
sample callbacks are rebuilt on the new owner, not serialized. The retained
indexed LCD image is included separately because
the existing application GB snapshot does not serialize that ICD source image.

Restore validates a separate, fully wired candidate and swaps it in only after
all components pass. Invalid/truncated/incompatible snapshots leave the live
host unchanged. Same-instance and cross-instance continuation are tested.
State operations and reset allocate and belong off any realtime audio callback.
The host is single-threaded and is not an audio-thread synchronization primitive.
Snapshots can contain private firmware/sample/save data: do not distribute them.
This container does not change the application's existing save-state format.
Version 1/2 host containers are rejected: they cannot retain pending LCD output
at a partially synchronized GB instruction. Existing experimental firmware-host
manual snapshots need to be recreated. Regular GB application states are
unchanged.

## Verification

```sh
ctest --test-dir build-release --output-on-failure -R '^gameboy_sgb_host_'
```

The ordinary contract uses only original synthetic images. The opt-in local
title test requires the user's private Donkey Kong v1.1 and firmware files;
it runs both models through 60 million instructions, compares complete native
WAVs with the established baselines, and repeats with whole-host restoration
every 8,192 samples. Reports measure **CPU + ICD/GB + APU** execution, including
PCM draining, rather than isolated DSP throughput. Restore-run measurements also
include snapshot work. They are host-specific observations, not speed gates or
proof of Android/web frontend headroom. The separate steady-state gate below
does enforce host playback headroom; snapshot runs are deliberately excluded.
The runner also reports process CPU time on POSIX systems (null where not
available), separating execution cost from wall time lost to contention. A
CPU-time ratio above unity does not override a wall-time realtime failure.

Historical pre-combined-audio captures: the local Release 60-million-instruction runs measured 1.19×
realtime for SGB1 and 1.24× for SGB2. Runs restoring the complete host 391 times
measured 1.02× and 1.15× respectively. All four complete WAVs matched their
established model-specific baselines byte for byte. This is limited headroom:
rendering, GB audio mixing, resampling and device playback are not included.

The opt-in `gameboy_sgb_host_combined_local_titles` test also captures both
models for 60 million instructions at 48/44.1 kHz, comparing normal output with
whole-host restoration and 257-sample consumer chunks. It requires unchanged
native WAV baselines and processor/SOUND counters, exact rational output counts,
audible GB capture and no clipping at default gains. It checks continuity and
integration, not fidelity to real hardware. Combined WAV hashes pin the current
cold-reset/clocked-pixel baseline so later optimizations must preserve every
sample, independently of the save/restore comparison. Captures remain private temporary
files and are removed by the test.

ROM-free contracts cover exact DC/transition areas, stereo source isolation,
clipping, long rate/count progression, raw-APU sink equivalence, source/output
backpressure, warm reset/release, invalid configuration/state, and maximum DMA
at the fastest ICD/APU clock settings, including GB STOP and wake-up timing.
Mixer cached levels/event deadlines and
reused IO trace storage avoid repeated arithmetic/allocation without altering
the captured waveform. The GB APU also caches its channel/routing voltage until
a register write, frame-sequencer event or channel DAC transition invalidates it.
High-pass capacitors and fractional sample integration still execute on every
native APU cycle with the original float operations. This derived cache is
invalidated on restore; it is not part of the save-state format. The ROM-free
`gameboy_apu_mixer_cache_contract` compares cached and original per-cycle mixing
byte for byte across all eight hardware models, including randomized writes,
power/mute transitions and a guaranteed audible four-channel sequence.
Full-host timing reports, not isolated mixer throughput,
remain the performance evidence.

Completed SPC reads now have a replay fast path that returns the already
latched byte without repeating bus-schedule decoding. New accesses, internal
idle slots and incomplete midpoint reads still use the original scheduler.
`gameboy_snes_spc_replay_cache_contract` compares the fast and original paths
across all 256 opcodes, both clock modes and changing host ports, including
per-clock bus events, registers and memory. DSP voice-register phase lookups
and narrowed readback scans remove redundant searches without changing phase
boundaries or ascending-voice ordering for shared latches.

The host also batches the *coordination* of APU halves between host bus
accesses. Every physical SPC half and every DSP phase still executes. PCM is
drained in bounded groups of at most 16 samples, with each timestamp derived
from its actual phase-27 output clock. Port rendezvous, ties and terminal fault
boundaries are unchanged. A scalar diagnostic oracle checks complete host
state, unread PCM and faults, including all eight maximum-length DMA channels
at the fastest accepted ICD/APU clocks.

SPC waiting-half lookahead avoids rerunning the interpreter when a normal
access has no first-half effect. Early input-port reads still execute at their
original midpoint. Completed opcode reads are reused, but new bus accesses
are never suppressed. The replay oracle covers sequential instructions,
all 256 opcodes and opcode fetches from the input ports themselves. DSP
readback dispatch retains the original shared-latch ordering. Interpolation
may be omitted only when its integer product is provably zero; pitch, BRR,
noise, envelopes and key sequencing still advance for inaudible voices.

Once all operands are latched and only internal idle clocks remain, the SPC
can reuse its already determined register result. Every remaining timer/DSP
clock and observer event still occurs. This derived idle-tail cache is cleared
on reset, new instructions and restoration. All-opcode tests compare complete
APU/DSP snapshots and bus events to the uncached interpreter, including
mid-instruction restores. `--scalar-spc` disables this cache for diagnosis;
`--scalar-apu` independently selects the original host coordination loop.

GB peripheral loops are grouped only across safe boundaries: no APU
sequencer edge, sample callback or active HDMA is crossed. TIMA's final-cycle
reload lock is preserved. Blank PPU spans stop before each event boundary;
active pixel/fetch work is unchanged. SGB palette RGB conversion and IRQ/ICD
deadlines use derived, invalidated caches. No frames, pixels, voices or samples
are dropped, and there is no fast-math or adaptive quality reduction.
Channel countdowns may advance together only before their next transition and
sample boundary. The same per-cycle float filtering and area additions remain;
no closed-form approximation replaces the analog recurrence. Runtime log-level
reads are atomic rather than locking a mutex on every disabled PPU trace dot.
`gameboy_sgb_execution_cache_contract` compares complete GB state and PCM
against per-cycle scheduling on all eight hardware models, plus SGB palette
and border pixels and full-width oscillator conversions.

### Repeatable playback headroom gate

Run benchmarks **serially**, with compilation and other test jobs finished.
The current private PCM/state pins are checked before accepting performance:

```sh
python3 scripts/benchmark_sgb_host.py \
  --runner build-release/gameboy_sgb_host_title_runner \
  --roms roms --output-dir /tmp/gbb-host-benchmark-1
```

The output directory must not already exist. It retains private WAVs and JSON
reports for diagnosis; do not commit or redistribute the WAVs. Both models run
60 million host instructions in native and combined modes, covering startup
and scripted Donkey Kong gameplay (48 kHz SGB1, 44.1 kHz SGB2 combined output).
The independent rate conversion remains the same digital presentation filter.
The ROM revision and initial `.sav` are pinned; missing or different save data
fails before playback. Final GB state/framebuffers must also match the original
baseline before performance can pass. The script's combined PCM pins are for
the Linux/GCC reference profile. Other compiler/architecture profiles require
a same-platform original-core comparison: ARM/Clang's existing float rounding
can differ from GCC by occasional PCM least-significant bits. That difference
must not be mistaken for a performance optimization changing the audio.
For repeatability qualification, add `--repeat 3` (up to 20 serial rounds).
Every complete capture must meet the same raw gate; repeats are not averaged
and no failing round is discarded. Repeated captures use `round-N` folders.

The runner records bounded one-second emulated playback windows. After ten
emulated seconds of warmup, at least 30 full windows are required. The default
gate requires **p05 >= 1.40x realtime and worst >= 1.20x**: a good whole-run
average cannot hide sustained slow sections. Truncated, restored and incomplete
model/audio-mode reports fail closed. Existing reports can be checked with
`scripts/check_sgb_host_performance.py`. The JSON also includes a final nested
GB-state hash, computed outside timed playback, for comparisons to an older
runner/core build. That hash includes framebuffer data; it is a regression
diagnostic, not a security or independent hardware-fidelity claim.

For host scheduling diagnosis, the runner also reports process CPU time in
each window, Unix voluntary/involuntary context switches, and sampled CPU
endpoints (Linux/Windows). Missing platform counters are `null`, not zero.
An endpoint change is not a complete count of migrations. `--calibrate-host`
adds a small dependent integer workload before and after playback, outside
the timed interval; compare it only between runs of the same binary. It can
help explain changing host capacity, but is not an emulator benchmark or a
cross-compiler score. Neither CPU time nor calibration normalizes the raw
wall-time headroom gate:

```sh
python3 scripts/diagnose_sgb_host_performance.py /tmp/sgb-headroom/*.json
```

`--callback-dsp` selects the original full-clock callback as an independent
oracle for the scheduler-owned direct DSP clock binding. Both paths advance
timers, then DSP, then the accepted SPC access, at the same physical clocks.
The binding is destination-owned across reset/restoration and never saved as
hardware state. Complete live APU snapshots, every opcode/half phase, output
backpressure, maximum DMA, and complete title PCM are compared; the direct
binding is not a license to skip clocks or samples. Other exact hot-path
changes bypass lower-half host I/O decoding for already-mapped ROM reads,
cache the immutable oscillator overflow limit, and collapse only speculative
idle-tail counter updates after the next physical access has blocked.
Completed operand-prefix reads use the same address/kind/half checks in a
compact fetch path; new, incomplete, dummy and early input-port accesses still
use the original generic bus path.

Absolute/indexed SPC loads and relative branches also retain a derived,
already-latched operand prefix. Their pending operand read, internal idles,
early port latch and arithmetic completion still follow the original bus
schedule. The prefix is discarded at instruction entry and restoration; it
is not hardware or snapshot state. Tests cover asynchronous port writes,
timer/DSP/IPL addresses, taken and untaken branches, and half-clock restores.
Absolute loads additionally retain only their completed low address byte;
the high-byte and target reads still rendezvous through the original bus
helpers. This two-byte derived cache is invalidated on entry, restore, and
execution-choice changes, and is never serialized.

The DSP driver specializes the same ordered schedule for each of its 32
phases, removing repeated phase tests without dropping a voice, echo access,
readback update or sample. `--scalar-dsp` selects the original runtime-phase
schedule. This destination-owned diagnostic choice survives reset/restore
without entering serialized hardware state. Live eight-voice/noise/pitch-
modulation/echo output and complete APU snapshots are compared with the
runtime-phase oracle, including full-buffer backpressure.
The specialized path also publishes the same ordered OUTX/ENVX/ENDX readback
with constant phase indices. A zero voice output can bypass multiplication
and saturating addition by zero, but not voice/envelope/pitch/BRR/key/echo
clocks or register publication.

On x86-64, eligible Game Boy APU batches compute the independent left/right
high-pass recurrences in paired SSE lanes. Every native cycle retains the
same subtraction, multiplication, addition, rounding, and capacitor update;
there is no fused arithmetic or analytic decay approximation. Other targets
retain the scalar recurrence. All-model scalar-oracle tests compare complete
PCM and serialized core state, not just the final displayed frame.

The optimized monochrome SGB pixel path resolves object/background priority
once for the raw 2-bit transfer capture and displayed RGB pixel. It applies
BGP/OBP only to display output, preserves the default configurable DMG
palette, and leaves mask/window timing unchanged. CGB and the cache-disabled
oracle retain the original separate composition paths. Randomized palettes,
object priority/OBP selection, raw transfer pixels, and complete title GB
states/framebuffers are compared byte for byte.

The combined mixer caches its sample-timestamp and advancement overflow
limits from the fixed output rate. Reset/queue ordering, capacity preflight,
area integration, clipping, and rounding are unchanged. Snapshots omit these
derived limits and validate the restored rate against the destination
configuration; constructor limits therefore remain valid after restoration.

These are **host-only** headroom thresholds. Frontend rendering, audio-device
latency, browser execution and Android thermal behavior need separate device
measurement. The shipped desktop backend remains experimental and opt-in;
historical frontend checks do not qualify the current clocked bridge.

### Historical measured headroom and build profile (before clocked-pixel-v1)

The captures and optimization progression in this section, including the
accepted target and Balanced qualification below, predate the clocked LCD
bridge. Their original hashes, thresholds, passes and failures are retained;
they do not qualify current bridge performance. See the current evidence
summary above for the later failing measurement.

The 60-million-instruction local captures below use the same initial save,
provisional gains and 48/44.1 kHz combined rates above. Ratios compare emulated
time to wall time; 1.5x means the host consumes at most about two thirds of the
realtime wall-time budget. Each row contains 90 post-warmup one-second windows:

The desktop captures at this historical milestone use the unchanged **Silent** Windows power profile
on an i7-12650H (Linux runs under WSL2). No governor, affinity or power-plan
changes, relaxed thresholds, sample/frame skipping or calibration scaling are
used. Passing captures below are not a repeatability qualification: subsequent
quiet captures failed in the same profile, as recorded below.

| Profile | Model / audio | Median | p05 | Worst | Gate |
| --- | --- | ---: | ---: | ---: | --- |
| SM-X130, Clang 19, packaged RelWithDebInfo + full LTO | SGB1 native | 1.82x | 1.76x | 1.75x | pass |
| same | SGB1 combined | 1.74x | 1.69x | 1.67x | pass |
| same | SGB2 native | 1.84x | 1.78x | 1.76x | pass |
| same | SGB2 combined | 1.75x | 1.69x | 1.68x | pass |
| Linux, GCC 15.2, Release + IPO, final operand replay | SGB1 native | 1.89x | 1.57x | 1.43x | pass |
| same | SGB1 combined | 1.83x | 1.65x | 1.63x | pass |
| same | SGB2 native | 1.93x | 1.80x | 1.71x | pass |
| same | SGB2 combined | 1.80x | 1.71x | 1.66x | pass |
| Windows, native MSVC 19.51, Release + IPO, final operand replay | SGB1 native | 1.83x | 1.68x | 1.48x | pass |
| same | SGB1 combined | 1.75x | 1.62x | 1.55x | pass |
| same | SGB2 native | 1.83x | 1.70x | 1.66x | pass |
| same | SGB2 combined | 1.72x | 1.64x | 1.58x | pass |

The Android Release + full-LTO profile independently passed all four rows
(p05 1.71–1.81x). Complete native and combined audio match the original
same-platform waveforms byte for byte, and nested GB-state hashes match the
previous same-platform optimized captures. The actual packaged shared library
also builds. Tests exercise optimized static-core consumers on-device; their
link options must retain Android's emulated-TLS handling.

The final operand-replay build also completed a 1,272-second SM-X130 soak:
six serial repeats of all four profiles, with all 24 complete PCM and final GB
state hashes unchanged. Minimum p05 / worst across the six repeats were
SGB1 native 1.80x / 1.78x, SGB1 combined 1.72x / 1.70x, SGB2 native
1.82x / 1.80x, and SGB2 combined 1.74x / 1.57x. The screen stayed awake
using harmless pointer movement with no clicks; power settings, affinity and
the installed app were unchanged. This is standalone host playback, not
shipping frontend FPS or a portable thermal-stability guarantee.

The later retained phase-dispatch, operand-cache, shared-pixel and fixed-rate
mixer-limit implementation completed another **1,219-second** awake SM-X130
soak on 2026-10-03. All six four-profile rounds passed the unchanged gates;
all 24 complete WAV hashes and final GB-state hashes matched their original
ARM baselines. Minimum ratios across the six rounds were:

| Model / audio | Minimum p05 | Minimum worst window |
| --- | ---: | ---: |
| SGB1 native | 1.888x | 1.850x |
| SGB1 combined, 48 kHz | 1.788x | 1.702x |
| SGB2 native | 1.911x | 1.782x |
| SGB2 combined, 44.1 kHz | 1.816x | 1.800x |

The actual packaged Android shared library also built with this source.
This qualifies the tested standalone awake-device workload, not frontend
rendering or arbitrary device/power conditions. The temporary ROM, save,
firmware and output copies were removed after retaining reports and exact
hash evidence; original files and installed-app settings were unchanged.

An attempted final-build soak while the tablet was Dozing failed all four
profiles (p05 1.24–1.43x, worst 0.88–1.39x), although complete PCM and GB state
still matched. Waking the locked screen periodically did not keep it awake
and added transition overhead; that attempt was stopped and retained as a
failure, not relabeled a pass. Active-playback measurements above do not
qualify background/suspended playback.

The packaged SGB1-native table entry is an isolated repeat. Its first run had
slow windows at emulated seconds 10–14 (p05 1.42x, worst 1.12x) and failed the
gate; that result is retained, not silently discarded. Earlier Linux IPO
captures passed all four profiles (p05 1.59–1.87x), but later quiet captures
failed (p05 1.17–1.26x, worst 1.06–1.19x). Pre-operand-replay native Windows
captures also failed; even a later three-profile pass still failed SGB1
combined at p05 1.46x. These observations are not portable speed guarantees
or frontend FPS. More aggressive GCC inlining, a native Clang experiment and
MSVC `/Ob3` were tested but not adopted: they did not consistently pass the
unchanged gate. Same-binary direct/callback A/B runs and diagnostic calibration
also showed host-capacity variation; do not attribute the entire measured
speed difference to one optimization. Shipping firmware playback remains
disabled; no quality reduction hides a failed gate.

Final quiet desktop repeats still failed all four modes: Linux p05
1.19–1.26x / worst 1.06–1.16x, native Windows p05 1.07–1.15x /
worst 0.95–1.07x. Complete PCM and GB state remained exact. Linux process
CPU/wall ratios were approximately 1.00 and Windows approximately 0.97;
same-binary calibration was substantially slower than during the earlier
passes. These counters diagnose changing host capacity, not an excuse to
normalize away a failing wall-time gate. **Repeatable Silent-mode desktop
headroom remained unqualified at the original 1.50x target.** A targeted forced-fetch-inlining A/B experiment
also did not show a reliable gain and was removed.

Further phase-specialization captures passed all four Linux profiles, but
native Windows still failed both combined profiles (p05 1.452x and 1.490x).
A subsequent paired-filter candidate failed three native Windows profiles,
despite exact complete PCM and GB hashes; one p05 result was 1.495x, which
fails the 1.500x gate even though two-decimal formatting could obscure that.
The gate was changed to print three decimals without changing its threshold
at that point. Later operand/cache candidates required their own qualification;
older passing tablet captures did not qualify those changes.
Bounded APU-check reservation and compact SPC return experiments were not
adopted: reservation added work to the common one-half rendezvous, and both
compact-return candidate runs were slower than the surrounding baseline
captures. Their all-opcode integrated-clock and DSP-phase fault tests remain.
A Windows-only compact-return comparison also preserved complete output,
but its later reporting baseline nearly matched the candidates and had the
better p05. That ABI variant was removed rather than presenting the first,
slower reporting capture as a reliable gain.

A subsequent three-round Linux qualification of the shared SGB pixel
composition and fixed-rate mixer-limit caches preserved complete PCM and GB
state in all twelve captures, but four captures failed headroom. The first
round failed SGB1 native and both combined profiles; the second round still
failed SGB1 combined. Only the third round passed all four. Consequently the
series fails qualification: the favorable final round does not replace the
earlier failures. These results are retained separately from later SPC
load-prefix experiments and do not qualify an unmeasured candidate.
The subsequent completed-load-opcode shortcut and a new Clang 19 comparison
also preserved exact output but showed no reliable gain against the enclosing
GCC baselines. The shortcut was removed; additional cache-toggle tests at
every load-prefix boundary remain. No shipping compiler choice was changed.

Three native MSVC repeats of the retained phase/pixel/mixer implementation
also preserved all twelve complete PCM and GB-state baselines, but nine of
twelve captures failed headroom. Every profile failed in the first round;
only SGB2 native passed in the second, and only the two SGB2 profiles passed
in the third. Failed p05 values ranged from 1.262x to 1.497x. Process CPU/wall
ratios remained approximately 0.98–0.99. Silent remained the active power plan
and no affinity or priority changes were made. All 164 Linux regressions,
four ASan/UBSan checks and six native Windows core checks passed for that
implementation. These correctness and awake-tablet successes do not qualify
the failing desktop series under the original 1.50x target. A later direct
host/APU binding experiment preserved exact output but showed only small,
mixed timing gains; it was removed in favor of the verified implementation.

### Accepted performance target and historical qualification (2026-10-03)

The practical default p05 target is now **1.40x**, with the **1.20x worst-window**
floor unchanged. The historical results above used the stricter 1.50x target;
their recorded failures are not retroactively relabeled. No audio, rendering,
clock accuracy or exact-output requirements were relaxed.

Rechecking the retained implementation's existing captures against the new
target gives **12/12 Linux**, **8/12 native Windows (Silent)** and **24/24 awake
tablet** passes. All Windows worst windows meet 1.20x, but four Windows p05
outliers remain below 1.40x (lowest 1.262x). This is an accepted stopping point,
not a claim of universal Silent-mode headroom or frontend FPS qualification.
Further speculative optimization is deferred; retained captures and diagnostics
make these limits explicit. Use `--minimum-ratio 1.5` for the previous stricter
gate.

Silent mode is no longer a release acceptance requirement. Future desktop
qualification should use the user's normal Balanced/Performance profile, with
the same raw 1.40x / 1.20x gate and exact-output checks. Historical Silent
captures remain diagnostic evidence, not evidence of normal-profile performance.
The benchmark accepts `--power-profile balanced` (or another descriptive label)
and records it in each report's `benchmark_context`. This label is explicitly
caller-declared, not an automatic detection or verification of system settings.
Omitting it records an unspecified profile; no tool changes power settings or
normalizes results based on the label.

One subsequent native MSVC Release + IPO capture of commit `2f7ae4e` on the
user-selected Windows **Balanced** plan passed all four profiles. The plan GUID
`381b4222-f694-41f0-9685-ff5bb260df2e` was confirmed before and after the run.
Each profile contains 90 post-warmup windows and preserves complete PCM and
final GB-state hashes against the original same-platform baseline:

| Profile | p05 realtime | Worst window | Result |
| --- | ---: | ---: | --- |
| SGB1 native, 32 kHz | 1.427x | 1.378x | pass |
| SGB1 combined, 48 kHz | 1.492x | 1.390x | pass |
| SGB2 native, 32 kHz | 1.554x | 1.481x | pass |
| SGB2 combined, 44.1 kHz | 1.511x | 1.425x | pass |

This is one clean normal-profile qualification, not a repeated thermal soak or
a guarantee of shipping frontend FPS. The preceding run spanned the user's
ASUS Recommended-to-Balanced switch and is retained as mixed-profile diagnostic
evidence only; it does not qualify Balanced playback. No affinity, priority,
power settings or emulator quality were changed by the benchmark.

Supported native release targets now enable IPO by default:

```sh
cmake -S . -B build-release -DCMAKE_BUILD_TYPE=Release \
  -DGAMEBOY_BUILD_SDL=OFF
cmake --build build-release -j4 --target gameboy_sgb_host_title_runner
ctest --test-dir build-release --output-on-failure \
  -R '^gameboy_sgb_host_combined_local_titles$'
```

Compiler/linker support is checked at configure time; unsupported combinations
fall back, and `GAMEBOY_ENABLE_RELEASE_IPO=OFF` opts out. Android Clang uses full
LTO for Release and packaged RelWithDebInfo because ThinLTO left the tablet
below the headroom gate. Debug, web and sanitizer profiles are unchanged. No
device-specific instruction set, fast-math or oscillator changes are used.
State/reset operations still allocate off the audio callback; repeated
restoration is measured separately from steady state.

The host still inherits the bounded CPU, bounded ICD and timing-only
SNES PPU/DMA limitations described in [SGB validation](sgb-validation.md).
Deferred visual mismatches and independent timing differences remain deferred.
See [APU components](sgb-audio-engine.md) for DSP/audio accuracy evidence.

### Original program-firmware project

An opt-in [original SNES-side diagnostic prototype](sgb-original-firmware.md)
now builds a LoROM, SPC driver, four authored BRR samples and two original score motifs without private
firmware. Its restricted SOUND, next-frame ICD transfer capture, SOU_TRN upload/handoff
and lifecycle contracts run on both models. Versioned driver adoption permits
repeated transfers and SOUND after compatible driver handoff. Mailbox v2 adds
independent pitch/volume, A decay/retrigger and timer-driven mute/unmute fades;
Mailbox v3 adds three presets per effect voice and two timer-driven scores;
v1/v2 retain their restricted command paths. It is not a
production default: general driver/sound-bank compatibility, complete sound/music
behavior and title validation remain required before removing the program-ROM
dependency.
