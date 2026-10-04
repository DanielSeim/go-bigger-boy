# Reusable SGB DSP audio components

The [bounded SGB firmware host](sgb-host.md) now owns the CPU, ICD/Game Boy
and APU scheduler together, with coordinated snapshots and bounded PCM output.
It remains opt-in; the desktop now has an explicit experimental firmware
playback adapter. Ordinary frontend launches still use HLE.
Its explicit combined-audio configuration now captures raw GB audio and
area-resamples the two sources on the host timeline. Gains remain provisional;
see the host document for buffering, snapshot and accuracy limitations.

GBB's validated DSP PCM renderer and 32-phase driver now live in
`gameboy_core`: `gameboy::SnesDspPcmRenderer` and `gameboy::SnesDspClock`.
The old diagnostic headers are compatibility aliases, so the synthetic
fixtures and independent title captures exercise the same core implementation.
The extraction changes neither the renderer arithmetic nor the phase order.

`gameboy::SnesDspAudioEngine` adds a bounded, caller-clocked wrapper around
those components. It is **not a running SGB SNES host by itself**; `SgbHost`
coordinates it with the CPU and GB bridge for experimental desktop playback.
It does not replace the Game Boy APU, load firmware automatically, or add a
bsnes runtime dependency. SGB music/effects require explicit user-owned firmware
and the experimental adapter; they are not enabled in ordinary launches.

## Integrated SPC700/APU scheduler

`gameboy::SnesApuAudioEngine` now runs our incremental SPC700 and the DSP
engine on one shared APU bus. It owns its components by default; the optional
SPC700 constructor attaches to a caller-owned CPU and its actual bus **at
reset**, for diagnostic host integration. Those external objects must outlive
the engine. A legally obtained IPL must be supplied by the caller; missing
IPL is reported explicitly without executing zero-filled memory. This still
does not execute a SNES CPU by itself; the bounded host supplies that scheduler.

`clock_half()` advances one physical SPC half clock. Input-port reads latch
on the first half; timers and one DSP phase advance on each completed full
clock, before its accepted SPC access. DSP-data writes are accepted exactly
once through the shared bus. A full 512-sample FIFO pauses **both** processors,
without issuing another read/write or losing PCM. Unsupported instructions
stop explicitly; reset is required to resume after a CPU fault.

`advance_to(master_clock, master_hz, apu_hz)` uses absolute integer half-clock
targets without accumulated rounding drift. Backward/overflowing targets and
invalid profiles are rejected. Drain PCM and retry the same target after
backpressure. The default remains 1,024,000 APU Hz; explicit oscillator profiles
are diagnostic inputs, not automatic SGB model detection. No DAC resampling,
wall-clock pacing or thread synchronization is performed here.

The scheduler reserves the CPU's full-clock observer and bus DSP-write
observer; callers must not replace these or clock the attached CPU separately.
The RAM-write/half-clock observers remain available for diagnostics. Restore
retains destination callbacks; reset clears bus observers and reattaches the
internal DSP route. Destruction detaches the reserved callbacks.

Composite state signature is `GBBSAPU` followed by version byte `1`. It contains
the DSP snapshot payload described below, plus explicitly encoded SPC registers,
cycle counters, instruction-start registers, all sixteen latched access slots,
continuation/half-clock flags, and scheduler status. Access slots store kind,
address, value, half count and early-read flag. Integer CPU counters use uint32;
cycle counts use uint64. Parsing validates a separate candidate before committing;
CPU/DSP clock agreement and replay/half-clock invariants are checked. The limit
is 71 KiB; restore and realtime clock/drain operations do not allocate.

This snapshot is complete for **this APU component**, not the whole SGB: it
excludes the SNES CPU, ICD/Game Boy timing and frontend queues/resampler. The
owner must restore those at the same boundary before resuming. Absolute master
targets and oscillator profiles belong to that owner, not this snapshot.
Firmware/RAM in snapshots remain private user data.

## DSP-only scheduling and buffering

Each successful `clock()` executes one DSP phase. DAC output occurs after
phase 27; one stereo sample is produced every 32 phases. The nominal output
rate is 32,000 Hz. The external scheduler determines the oscillator rate;
the engine does not read wall time, change clocks or resample to the
frontend's 48 kHz mixer.

The FIFO holds at most **512 stereo samples** (16 ms at the nominal rate).
When full, `clock()` returns false without changing state, and `run_clocks()`
returns the actual number of phases advanced. Samples are never overwritten
or dropped. Drain with `pop_sample()`, then resume the unconsumed clock budget.
An empty pop returns false without altering the destination sample.

Clocking and draining use fixed storage and do not allocate. State export is
an allocating, off-audio-path operation. The engine is **single-threaded**:
callers must coordinate bus writes, clocking, draining and state operations;
it is not an audio-thread synchronization primitive. A future host must stop
or drain this DSP together with its CPU scheduling when backpressure occurs.

`write_dsp()` is for standalone callers; it writes the APU ports and updates
synthesis side effects. A shared SPC bus caller instead commits the write
once, then invokes `accept_dsp_write()` once. Do not invoke both for the same
write, or attach a callback that recursively calls `write_dsp()`.

## Reset and component snapshots

`reset()` flushes queued output and resets voice synthesis, key polling, echo
history and phase. It preserves the shared APU RAM, programmed registers,
timers, IPL and observer registrations. DSP-owned ENVX/OUTX/ENDX readback is
cleared. For a complete APU memory reset, explicitly reset the caller-owned
bus as well, then reset/configure the engine. Neither operation claims to
replace SNES CPU/SPC700 hardware reset sequencing.

The component's `save_state()` returns a version-1 little-endian byte stream:

- Eight-byte signature `GBBSDSP` followed by version byte `1`.
- Shared APU RAM, DSP registers, communication ports, timer state, supplied
  IPL bytes and overlay/selector flags. Observers and callback contexts are
  deliberately excluded.
- All eight BRR predictors, group cursors, sample rings, envelopes and key-on
  sequences; global key/rate/noise state; staggered register/readback latches;
  pending mixes and echo writes/history; phase-driver counters/volume latches.
- FIFO head, count and all 512 stereo slots.

Integers use explicit widths; enums and booleans use one byte. No compiler
padding, object references, file paths or function pointers are serialized.
Changing this layout requires a new version. States may contain private
firmware/sample data supplied by the user: **do not commit or distribute
captured states** as reference fixtures.

`load_state()` checks signature/version, exact consumption, a 70 KiB input
bound, boolean encodings and internal cursor/counter ranges before changing
live state. It returns false on an invalid state, does not allocate, does not
invoke bus observers, and retains existing callback registrations on success.
BRR streams remain bound to the destination bus after cross-instance restore.
This is structural validation, not a cryptographic integrity check.

This is **not an application save state**: it does not include SPC700 or
65C816 registers/in-flight instructions, ICD/GB scheduling or frontend audio
queues. A future live host must coordinate these with this component snapshot
at a common boundary. The emulator's existing save-state format is unchanged.

## Verification

```sh
ctest --test-dir build-release \
  -R 'gameboy_snes_(apu_audio_engine|dsp_(audio_engine|pcm_renderer))' \
  --output-on-failure
```

Original synthetic tests cover all 64 phase/key-poll positions, non-silent
stereo continuation after same- and cross-instance restore, live noise/pitch
modulation/envelopes/echo and dynamic key/volume/register changes, pending
echo writes, wrapped/full/empty FIFO behavior, allocation-free clock/restore
paths, reset repeatability, observer preservation and atomic malformed-state
rejection. Existing independent DSP fixtures and local title PCM contracts
remain the extraction's audio-regression checks.

The integrated tests additionally cover all 128 half-clock positions twice
(startup and active echo/envelopes),
including pending opcode/operand reads and DSP stores, full FIFO pauses,
allocation-free restore/clock/drain, invalid CPU replay states, destination
observer preservation and exact integer rendezvous. A SPC-generated eight-voice
stream matches the independent DSP reference byte-for-byte (256 stereo outputs,
SHA-256 `ded49d6cc25f85de36ed2768075fb889d59b383b45817bae905023638fe70371`).
CI pins that hash and compares against the existing renderer protocol; the
independent source stays external and is only used when explicitly provided:

```sh
python3 tests/snes_apu_audio_engine_pcm_tests.py \
  build-release/gameboy_snes_apu_audio_engine_tests \
  build-release/gameboy_snes_dsp_pcm_fixture_runner \
  --reference-dir /path/to/external/SPC_DSP/source
```

The diagnostic SNES trace accepts `--core-apu-engine` for PCM/RAM capture on
the existing fractional host schedule, without running a second DSP renderer.
Legacy bus/state tracing options are rejected in this mode. Optional local-ROM
tests compare complete native WAVs against the established diagnostic path:
SGB1 and SGB2 Donkey Kong gameplay, each through 60 million host instructions.
The bounded diagnostic host now supports binary `ADC abs,X` (`$7d`) and
`STA (dp),Y` (`$91`), which previously prevented SGB1 from releasing the GB.
No change to the ICD run-control bit was needed. Decimal indexed ADC still
fails closed, and this is not a complete SNES CPU implementation.
`--allow-unanchored-pcm` remains an explicit startup-only diagnostic option;
the title regression no longer uses it. Integration parity does not resolve
the documented independent title timing/waveform differences or establish
hardware-perfect audio.
The local integration test enables `--core-apu-state-roundtrip`, restoring the
live APU component every 8,192 native samples while requiring unchanged WAV
bytes. No private snapshot bytes are written to disk.

The original eight-voice synthetic workload also reports measured native PCM
throughput after warmup (no resampling or frontend). This is a desktop diagnostic,
not a fixed cross-platform FPS gate or an Android/web performance guarantee.

`--core-apu-benchmark-output /path/to/new-report.json` additionally captures a
private live APU state 8,192 native samples after the first audible SOUND
delivery and replays it three times, for two emulated seconds per trial. The
report contains wall time, realtime ratio, sample counts and a PCM hash; no
firmware, state or audio bytes are persisted by the benchmark. The local title
test requires 64,000 outputs per trial at the nominal clock, nonsilent PCM,
and identical output hashes across all three restored continuations. It
measures the real firmware/SOUND workload in isolation: no GB/SNES host CPU,
future host commands, frontend, audio device or resampling. Measured ratios
are reported, not used as a machine-dependent CI speed threshold.

One local Release run, concurrent with the regression suite, measured SGB1
at 3.07×, 2.94× and 3.07× realtime, and SGB2 at 2.80×, 2.97× and 3.23×.
Both produced 64,000 native stereo outputs per trial with repeatable hashes.
These are host-specific isolated APU measurements, not end-to-end emulator
frame rates or evidence that Android/web already have sufficient headroom.

See [SGB validation](sgb-validation.md) for the independent capture evidence
and remaining timing/host limitations. These tests do not establish complete
S-DSP hardware accuracy or validate physical audio-device playback. Experimental
desktop firmware playback has its own integration checks in the host document.
