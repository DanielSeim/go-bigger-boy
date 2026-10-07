# Experimental calibrated articulation gates

`firmware/sgb/score_gate.asm` adds early DSP key-off to the isolated
[owned-waveform renderer](sgb-native-score-render.md). The implementation is an
independently specified, explicitly calibrated duration-16 profile supported by
controlled black-box observations. It does not establish the original gate
algorithm or a general duration/articulation law. It remains excluded from
bundled firmware; SGB1/SGB2 program images remain required for production.

## Supported profiles

| Tempo | Articulation | Note duration | Timer pulses to key-off |
| --- | --- | --- | --- |
| 96 | `$7F` | 16 ticks | 36 |
| 192 | `$7F` | 16 ticks | 18 |
| 96 | `$3F` | 16 ticks | 23 |

These pulse counts were selected from the earlier controlled onset/gate timing
ranges, then validated with fresh original-program executions. They are
calibration parameters, not independently confirmed original implementation
constants. One timer pulse spans 2048 SPC cycles. Measured key-on-to-key-off
intervals are slightly shorter than pulse count times 2048 because native note
setup occurs after the timer pulse that dispatches the event.

The existing caller interface, three pitches, owned BRR source and direct gain
remain. Notes require duration 16; rests may use duration 2..127. Articulation
may be inherited or explicitly changed between the two supported values at
tempo 96. `$3F` at tempo 192, other articulation values, other note durations,
unsupported opcodes and malformed streams reject during full validation,
before any event, key-on or nonzero PCM. No interpolation is attempted.

`$2D` holds a private timer-pulse countdown. A key-on starts the selected gate;
each consumed timer pulse decrements it before advancing the fractional score
clock. At zero, the SPC asserts voice-2 key-off without changing track duration
or the next onset. Rest, transition and completion clear the countdown.
Pending pulses are consumed individually; as with the clock, an overflowing
four-bit timer output counter is not recovered. The directory, waveform,
instrument setup and diagnostic memory layout remain separate from production
firmware ownership.

## Build and validation

```sh
python3 scripts/build_sgb_score_gate.py --output /tmp/score-gate.bin
cmake --build build-dmg-firmware --target gameboy_sgb_score_gate_probe
ctest --test-dir build-dmg-firmware -R '^gameboy_sgb_native_score_gate$' --output-on-failure
python3 scripts/check_sgb_score_gate_reference.py \
  --probe build-dmg-firmware/gameboy_sgb_score_gate_probe \
  --trace build-dmg-firmware/gameboy_snes_65c816_apu_trace --firmware-dir roms
```

The checked source hooks retain the standalone clock, parser and full-duration
renderer artifacts unchanged. The gated artifact remains 2073 bytes at `$0800`,
SHA-256 `d6d5e71c079c3d65db9c579a3735a802303712736f59626d13504a82e2b33fde`.
No private code or samples are included. Output files must not already exist.

The native probe observes actual DSP register key-on/key-off transitions
through the production component without replacing its owned DSP observer.
It retains instrument, stereo PCM, release-tail, mailbox isolation and complete
input rejection checks. Reset must reproduce event/key-on/key-off timestamps
and owned PCM fingerprints. Whole-engine save/load checks additionally cover
active gate countdowns and the first key-off boundary, including instruction
half-clock continuation and release PCM. Mixed-articulation and inherited
state align with the separate symbolic scheduler. Rest-only streams stay silent.

The private gate reuses six fresh original SGB1/SGB2 timing runs: baseline,
double tempo and short articulation. Native owned fixtures begin with a
16-tick rest, then play three duration-16 notes and an eight-tick rest. For each
case, two onset intervals and all three key-on-to-key-off gates must match the
corresponding sanitized reference observation within 2048 SPC cycles. Existing
reference timing matrix checks also run; failed or incomplete observations do
not produce a success report. Reports explicitly mark both `qualification`
and `playback` false, record calibration parameters and hashes, and include
only sanitized reference timing and owned PCM statistics.

The DSP release envelope and waveform are independently defined. Register gate
agreement does not imply original PCM, original envelope shape, startup phase,
other durations or tempo combinations, whole SGB boot, or complete vendor
playback. Broader timing needs additional controlled fixtures before expanding
this profile; instrument/control mapping and multi-track integration also
remain separate work.

The [expanded controlled timing matrix](sgb-expanded-gate-timing.md) measures
additional durations/tempos separately from this native profile.
