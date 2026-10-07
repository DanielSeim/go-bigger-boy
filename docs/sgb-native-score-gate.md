# Experimental calibrated articulation gates

`firmware/sgb/score_gate.asm` adds early DSP key-off to the isolated
[owned-waveform renderer](sgb-native-score-render.md). The implementation uses
an independently specified, explicitly calibrated profile table supported by
controlled black-box observations. It does not establish the original gate
algorithm or a general duration/articulation law. It remains excluded from
bundled firmware; SGB1/SGB2 program images remain required for production.

## Supported profiles

| Tempo | Articulation | Note duration | Timer pulses to key-off |
| --- | --- | --- | --- |
| 96 | `$7F` | 8 ticks | 15 |
| 96 | `$3F` | 8 ticks | 10 |
| 96 | `$7F` | 16 ticks | 36 |
| 96 | `$3F` | 16 ticks | 23 |
| 96 | `$7F` | 24 ticks | 58 |
| 96 | `$3F` | 24 ticks | 37 |
| 128 | `$7F` | 16 ticks | 27 |
| 128 | `$3F` | 16 ticks | 17 |
| 192 | `$7F` | 16 ticks | 18 |
| 192 | `$3F` | 16 ticks | 11 |

Pulse counts were selected from the controlled onset/gate timing ranges, then
validated with fresh original-program executions. They are calibration
parameters, not independently confirmed original implementation constants.
One timer pulse spans 2048 SPC cycles. Measured key-on-to-key-off intervals are
slightly shorter than pulse count times 2048 because native note setup occurs
after the timer pulse that dispatches the event. A bounded lookup of ten
four-byte records at `$0E00` selects an exact tempo/articulation/duration match;
unknown combinations reject. No interpolation or proportional gate scaling
is attempted.

The existing caller interface, three pitches, owned BRR source and direct gain
remain. Rests may use duration 2..127 and articulation `$3F` or `$7F`. At tempo
96, streams may mix note durations 8/16/24 and both articulations, retaining
inherited parser state. Tempos 128/192 support duration-16 notes only. Other
articulations, note durations, unsupported opcodes and malformed streams reject
during full validation, before any event, key-on or nonzero PCM.

The gated build extends the fractional clock's accepted tempos to 96/128/192.
At tempo 128, the retained phase model generates one score tick for every two
timer pulses. Standalone clock, parser and full-duration renderer builds keep
their previous tempo limits and image hashes. Only the gated diagnostic build
uses this expanded clock validation.

`$2D` holds the private timer-pulse countdown, `$2E` the selected pulse count,
and `$2F` the bounded table cursor. A key-on starts the selected gate; each
consumed timer pulse decrements it before advancing the fractional score clock.
At zero, the SPC asserts voice-2 key-off without changing track duration or the
next onset. Rest, transition and completion clear the countdown. Pending
pulses are consumed individually; an overflowing four-bit timer output counter
is not recovered. Diagnostic memory remains separate from production ownership.

## Build and validation

```sh
python3 scripts/build_sgb_score_gate.py --output /tmp/score-gate.bin
cmake --build build-dmg-firmware --target gameboy_sgb_score_gate_probe
ctest --test-dir build-dmg-firmware \
  -R '^gameboy_sgb_(expanded_)?native_score_gate$' --output-on-failure
python3 scripts/check_sgb_expanded_score_gate_reference.py \
  --probe build-dmg-firmware/gameboy_sgb_score_gate_probe \
  --trace build-dmg-firmware/gameboy_snes_65c816_apu_trace --firmware-dir roms
```

The gated artifact remains 2073 bytes at `$0800`, SHA-256
`31b3ac1435eaec562274803d7106ebd2bb0408478e4a469c81dadb0c90bdf5f9`.
No private code or samples are included. Existing output files are preserved,
and bundled firmware is unchanged.

The native probe observes actual DSP register key-on/key-off transitions
through the production component without replacing its owned DSP observer.
It retains instrument, stereo PCM, release-tail, mailbox isolation and complete
input rejection checks. Reset must reproduce event/key-on/key-off timestamps
and owned PCM fingerprints. Whole-engine save/load checks cover active gate
countdowns and the first key-off boundary, including instruction half-clock
continuation and release PCM. Mixed duration/articulation and inherited state
align with the separate symbolic scheduler. Rest-only streams stay silent.
Tempo-128 checks include sixteen notes and low-counter-byte carry at tick 256.
Unsupported combinations after a valid prefix must reject the whole stream.

The expanded private gate validates twenty fresh original SGB1/SGB2 timing
runs across all ten profiles. It checks the existing three-profile matrix and
[expanded controlled timing matrix](sgb-expanded-gate-timing.md), reusing the
same baseline control observation. Native fixtures begin with a rest of the
tested duration, then play three notes and an eight-tick rest. For each case,
two onset intervals and all three key-on-to-key-off gates must match sanitized
reference observations within 2048 SPC cycles. Incomplete or failed matrices
do not produce a success report. The earlier three-profile checker remains
available as a regression subset.

Reports mark `qualification` and `playback` false, record explicit profile
parameters and hashes, and include sanitized reference timing and owned PCM
statistics. Register timing agreement does not imply original PCM, envelope
shape, startup phase, untested parameter combinations, whole SGB boot, or
complete vendor playback. Instrument/control mapping and multi-track
integration remain separate work.
