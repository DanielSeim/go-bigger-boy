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
no wall-clock sleep, audio device or thread synchronization. Native output is
not resampled; the opt-in combined path below performs digital rate conversion.

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

The default output remains **SNES-side PCM only**, with the unused GB analog
mixer/output queue disabled. The existing frontend rendering/presentation paths
are unchanged. Combined output is a separate explicit configuration below;
device playback remains future work.

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

`save_state()` and `load_state()` use `GBBSHOST`, version 2, with explicit
little-endian scalar fields and a 16 MiB maximum. No object padding, callbacks,
addresses or filesystem paths are serialized. A configuration fingerprint
binds the images, model, clock profile, initial save, input script, audio mode,
output rate and gains. An FNV
payload checksum catches accidental corruption; neither is a security boundary.

Snapshots include host CPU registers, WRAM, DMA, interrupt/math/PPU timing;
the complete APU state; GB emulator state; ICD packet assembly, queued and
latched packets, indexed LCD/row buffers, release clock and input cursor; and
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
Version 1 diagnostic host containers are rejected; this opt-in container has
not been a frontend save format.

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
The runner also reports process CPU time on POSIX systems (null where not
available), separating execution cost from wall time lost to contention. A
CPU-time ratio above unity does not override a wall-time realtime failure.

Before combined audio was added, the local Release 60-million-instruction runs measured 1.19×
realtime for SGB1 and 1.24× for SGB2. Runs restoring the complete host 391 times
measured 1.02× and 1.15× respectively. All four complete WAVs matched their
established model-specific baselines byte for byte. This is limited headroom:
rendering, GB audio mixing, resampling and device playback are not included.

The opt-in `gameboy_sgb_host_combined_local_titles` test also captures both
models for 60 million instructions at 48/44.1 kHz, comparing normal output with
whole-host restoration and 257-sample consumer chunks. It requires unchanged
native WAV baselines and processor/SOUND counters, exact rational output counts,
audible GB capture and no clipping at default gains. It checks continuity and
integration, not fidelity to real hardware. Combined WAV hashes also pin the
initial uncached implementation so later optimizations must preserve every
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

### Measured headroom and build profile

The 60-million-instruction local captures on Linux/GCC 15.2 measured the
following wall-time ratios (higher than 1 means faster than emulated realtime).
The combined runs use the same provisional gains and 48/44.1 kHz rates above:

| Model | Release native | Release combined | Release + IPO native | Release + IPO combined |
| --- | ---: | ---: | ---: | ---: |
| SGB1 | 0.87x | 0.83x | 1.04x | 0.98x |
| SGB2 | 0.87x | 0.84x | 1.12x | 1.17x |

Both profiles preserve the complete native and original combined waveform
hashes. These are observations, not portable speed guarantees or a reliable
relative-cost estimate: clock frequency and run-to-run variation affect them.
Process CPU and wall time were nearly equal in the steady-state captures.
**This is not sufficient cross-platform playback headroom**: even the IPO
SGB1 run missed realtime, and rendering/device playback are not included.
Shipping playback therefore remains disabled. Further host/SPC scheduling
work is needed before frontend integration; audio quality is not reduced to
hide the performance limit.

The separate diagnostic IPO build used CMake's existing opt-in property:

```sh
cmake -S . -B build-host-ipo -DCMAKE_BUILD_TYPE=Release \
  -DGAMEBOY_BUILD_SDL=OFF -DCMAKE_INTERPROCEDURAL_OPTIMIZATION_RELEASE=ON
cmake --build build-host-ipo -j4 --target gameboy_sgb_host_title_runner
ctest --test-dir build-host-ipo --output-on-failure \
  -R '^gameboy_sgb_host_combined_local_titles$'
```

This requires a compiler/linker supporting IPO. No global build defaults or
fast-math flags were changed. State/reset operations still allocate off the
audio callback; repeated restoration is measured separately from steady state.

The host still inherits the bounded CPU, scanline-granular ICD and timing-only
SNES PPU/DMA limitations described in [SGB validation](sgb-validation.md).
Deferred visual mismatches and independent timing differences remain deferred.
See [APU components](sgb-audio-engine.md) for DSP/audio accuracy evidence.
