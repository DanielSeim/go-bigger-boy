# Reusable SGB DSP audio components

GBB's validated DSP PCM renderer and 32-phase driver now live in
`gameboy_core`: `gameboy::SnesDspPcmRenderer` and `gameboy::SnesDspClock`.
The old diagnostic headers are compatibility aliases, so the synthetic
fixtures and independent title captures exercise the same core implementation.
The extraction changes neither the renderer arithmetic nor the phase order.

`gameboy::SnesDspAudioEngine` adds a bounded, caller-clocked wrapper around
those components. It is **not a running SGB SNES host**, and no frontend
instantiates it yet. This work does not enable SGB music/effects in releases,
replace the Game Boy APU, load firmware automatically, or add a bsnes runtime
dependency. It is a building block for a future original sound host.

## Scheduling and buffering

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
  -R 'gameboy_snes_dsp_(audio_engine|pcm_renderer)_contract' \
  --output-on-failure
```

Original synthetic tests cover all 64 phase/key-poll positions, non-silent
stereo continuation after same- and cross-instance restore, live noise/pitch
modulation/envelopes/echo and dynamic key/volume/register changes, pending
echo writes, wrapped/full/empty FIFO behavior, allocation-free clock/restore
paths, reset repeatability, observer preservation and atomic malformed-state
rejection. Existing independent DSP fixtures and local title PCM contracts
remain the extraction's audio-regression checks.

See [SGB validation](sgb-validation.md) for the independent capture evidence
and remaining timing/host limitations. These tests do not establish complete
S-DSP hardware accuracy or enable audible SGB sound in shipping frontends.
