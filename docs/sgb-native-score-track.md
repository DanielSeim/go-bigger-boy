# Experimental native single-track scheduling

`firmware/sgb/score_track.asm` connects an independently written flat-track
parser to the [native SPC score clock](sgb-native-score-clock.md). The production
SPC CPU reads the supplied bytes, retains duration/articulation, waits for score
ticks and writes timed event records. The host probe only installs inputs and
observes memory. It does not parse or schedule events on behalf of the SPC.

This standalone experiment remains excluded from bundled firmware. It keeps
the DSP muted/reset and mailbox ports quiet; it emits no audible notes and does
not claim production ownership. SGB1/SGB2 program ROMs remain required.

## Supported stream

The caller provides tempo 96 or 192 at direct-page `$12`, length 1..128 at `$20`,
and a flat track at `$2B00`. The first timed event requires explicit duration
1..127 and articulation 0..127. Subsequent events may inherit both; a new
duration may omit articulation and reuse the previous value. Notes `$80..C7`
and rest `$C9` consume the current duration. These are syntax ranges, not pitch
or articulation-gate qualification. A single zero must close the stream exactly
at its supplied end, after at least one timed event.

All controls, ties, calls, tables, multiple channels and phrase transitions
are unsupported. The firmware validates the entire stream before starting the
timer or emitting records. It rejects missing/incomplete state, unsupported
opcodes, trailing data and more than 16 events. Cursor bounds limit reads to the
supplied stream, and the event limit bounds total score ticks to 2032. Only the
probe directly installs input; this does not validate IPL upload or SGB boot.

The parser uses private direct-page `$21..29` and `$2B`, plus the clock's
`$10..15`. Five-byte records at `$3000` contain little-endian score tick, raw
opcode, duration and articulation, with at most 80 bytes written. `$28` is the
record write offset; complete records end at multiples of five. Status `$14`
is 1 running, 2 complete, `$E1` rejected tempo or `$E2` rejected stream. These
addresses form a diagnostic interface, not an integrated firmware ABI. Completion
and rejection disable timers and enter a silent halt loop.

## Build and validation

```sh
python3 scripts/build_sgb_score_track.py --output /tmp/score-track.bin
cmake --build build-dmg-firmware --target gameboy_sgb_score_track_probe
ctest --test-dir build-dmg-firmware -R '^gameboy_sgb_native_score_track$' --output-on-failure
python3 scripts/check_sgb_score_track_reference.py \
  --probe build-dmg-firmware/gameboy_sgb_score_track_probe \
  --trace build-dmg-firmware/gameboy_snes_65c816_apu_trace --firmware-dir roms
```

The builder checks two exact hooks in the owned clock source and appends the
parser source, using the existing strict assembler. It does not modify the
standalone clock image. Output SHA-256 is
`8674090e26739e16f24a4451206b7321e973a0031028a1a68d584d98281caa27`;
output files must not already exist. The bundled prototype remains unchanged.

ROM-free checks align native event kinds, durations, articulation and end ticks
with the separate [symbolic scheduler](sgb-vendor-score-scheduler.md), using an
owned one-pattern/channel-2 wrapper. Cases include inherited state, rests,
minimum and maximum durations, a 16-event boundary and counter carry. Native
runs must reproduce physical-half event timestamps after reset and whole-engine
save/load at four checkpoint positions, with silent PCM and zero mailbox ports.
Malformed streams and unsupported tempos must reject before producing events.

The optional private gate compares three owned native timing cases against
six fresh original SGB1/SGB2 runs: baseline, double tempo and short articulation.
Native streams add a duration-16 warmup rest before three notes to exclude
startup phase from the comparison. Native event spacings are compared with
original DSP note-onset spacings within one timer pulse (2048 SPC cycles).
Articulation is recorded, but the native parser does not produce key-off gates;
only onset spacing is compared. This is bounded timing evidence, not PCM,
initial note-phase or complete vendor playback qualification.

Reports retain `qualification: false` and `playback: false` and export only
owned event metadata, sanitized reference timing and input hashes. The next
step is native instrument setup and DSP rendering for an owned single-track
fixture; broader grammar and firmware integration require further validation.

An isolated [native DSP renderer](sgb-native-score-render.md) now exercises
three pitches with an owned waveform, separately from production ownership.
