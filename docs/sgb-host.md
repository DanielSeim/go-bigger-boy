# Bounded SGB firmware host

`gameboy::SgbHost` coordinates our original bounded SNES CPU, ICD/Game Boy
bridge and SPC700/DSP engine in `gameboy_core`. It is an opt-in core component,
**not enabled in shipping frontends**. No bsnes code, external emulator core,
Nintendo firmware, ROM, captured audio or private state is bundled.

The existing diagnostic CPU/ICD headers are compatibility adapters to the same
core implementation. Legacy timing profiles and tests remain available;
moving the code does not make it a complete SNES emulator.

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
no wall-clock sleep, resampling, audio device or thread synchronization.

Unknown CPU/SPC instructions and missing ICD data stop explicitly. Faults stay
terminal until reset. `fault()` retains host instruction/address context;
`status()` distinguishes host, APU and ICD failures. `reset()` reconstructs a
cold host using the same configured images, input script and initial save RAM.

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

Output is **SNES-side PCM only**. GB channel clocks/registers continue running,
but the unused GB analog mixer/output queue is disabled. Combining both audio
sources, frontend-rate conversion and device playback are separate future work.
The existing frontend rendering/presentation paths are unchanged.

## Whole-host snapshots

`save_state()` and `load_state()` use `GBBSHOST`, version 1, with explicit
little-endian scalar fields and a 16 MiB maximum. No object padding, callbacks,
addresses or filesystem paths are serialized. A configuration fingerprint
binds the images, model, clock profile, initial save and input script. An FNV
payload checksum catches accidental corruption; neither is a security boundary.

Snapshots include host CPU registers, WRAM, DMA, interrupt/math/PPU timing;
the complete APU state; GB emulator state; ICD packet assembly, queued and
latched packets, indexed LCD/row buffers, release clock and input cursor; and
unread host PCM. The retained indexed LCD image is included separately because
the existing application GB snapshot does not serialize that ICD source image.

Restore validates a separate, fully wired candidate and swaps it in only after
all components pass. Invalid/truncated/incompatible snapshots leave the live
host unchanged. Same-instance and cross-instance continuation are tested.
State operations and reset allocate and belong off any realtime audio callback.
The host is single-threaded and is not an audio-thread synchronization primitive.
Snapshots can contain private firmware/sample/save data: do not distribute them.
This container does not change the application's existing save-state format.

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
proof of Android/web frontend headroom.

On the local Release build, the 60-million-instruction runs measured 1.19×
realtime for SGB1 and 1.24× for SGB2. Runs restoring the complete host 391 times
measured 1.02× and 1.15× respectively. All four complete WAVs matched their
established model-specific baselines byte for byte. This is limited headroom:
rendering, GB audio mixing, resampling and device playback are not included.

The host still inherits the bounded CPU, scanline-granular ICD and timing-only
SNES PPU/DMA limitations described in [SGB validation](sgb-validation.md).
Deferred visual mismatches and independent timing differences remain deferred.
See [APU components](sgb-audio-engine.md) for DSP/audio accuracy evidence.
