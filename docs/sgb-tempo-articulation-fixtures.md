# Controlled vendor tempo and articulation observations

The [pitch/instrument fixture](sgb-pitch-instrument-fixtures.md) pins three notes
on channel 2. This fixture plays those notes in one song and changes one control
at a time to measure onset spacing and key-on-to-key-off intervals.

```sh
python3 scripts/build_sgb_timing_fixture.py --case baseline --output /tmp/timing.gb
python3 scripts/build_sgb_timing_fixture.py --case double-tempo --output /tmp/timing-fast.gb
python3 scripts/build_sgb_timing_fixture.py --case short-gate --output /tmp/timing-short.gb
python3 scripts/check_sgb_timing_reference.py --trace build-dmg-firmware/gameboy_snes_65c816_apu_trace --firmware-dir roms
```

Each independently authored cartridge uses the shared valid header, VRAM
transport and actual JOYP code. The 54-byte bank at `$2B00` contains one phrase
and one eight-channel pattern with only channel 2 populated. The track sets
instrument 2, pan 10, song volume 160, track volume 127, tempo, duration 16 and
articulation, then notes 24, 25 and 36, a duration-1 rest and an end. The duration
and articulation persist across notes. There is only one SOUND request, ID 1;
subsequent music requests cannot interrupt the measured song. No reference
instrument table, BRR sample, score or firmware bytes are included.

| Case | E7 tempo | Articulation | Change from baseline |
| --- | --- | --- | --- |
| baseline | 96 | `$7F` | None |
| double-tempo | 192 | `$7F` | Tempo operand only |
| short-gate | 96 | `$3F` | Articulation operand only |

The lower articulation nibble remains `$F`; these comparisons do not establish
a velocity mapping.

## Measured contract, 2026-10-06

Each case was run against both private original program images and the private
original SPC IPL with GBB's independent SGB1/SGB2 GB bootstrap. All notes selected
physical DSP voice 2 with SRCN 2, no noise, and the preceding milestone's pitch
words 1068, 1132 and 2140. The observer pairs each nonzero KON with the first
subsequent accepted KOF write whose voice-2 bit is set. Repeated KOF writes do
not extend the interval. The next onset requires a preceding key-off.

The following values are **SPC execution cycles between accepted DSP writes**.
They are not PCM onset/release measurements, audible note lengths, or hardware
accuracy claims.

| Model | Case | Two onset intervals | Three key-on-to-key-off intervals |
| --- | --- | --- | --- |
| SGB1 | baseline | 86622, 87594 | 74848, 73779, 74261 |
| SGB2 | baseline | 86166, 88049 | 74383, 74233, 74259 |
| SGB1 | double-tempo | 43155, 43454 | 35475, 35324, 36941 |
| SGB2 | double-tempo | 43151, 42994 | 35885, 35327, 37400 |
| SGB1 | short-gate | 86625, 87589 | 45712, 47608, 47634 |
| SGB2 | short-gate | 86161, 88052 | 45708, 47612, 47630 |

For these cases, doubling tempo reduces onset intervals to 0.488..0.501 of
baseline. Changing articulation `$7F` to `$3F` changes onset spacing by at most
five cycles within each model and reduces key-on-to-key-off intervals to
0.611..0.645 of baseline. Corresponding model intervals differ by at most 465
cycles. These observations support separate onset scheduling and articulation
gates in a future renderer, but do not determine the full timer, fractional
accumulator, quantization table or lookahead algorithm.

The regression gate requires baseline onset intervals of 84000..90000 cycles,
double-tempo onset ratios 0.47..0.53, short-gate onset ratios 0.97..1.03 and
short-gate duration ratios 0.59..0.67. Absolute gate bounds are 72000..76000 for
baseline, 34000..39000 for double-tempo and 44000..49000 for short-gate. Matching
model intervals may differ by at most 2048 cycles. These are explicitly chosen
bounds around this execution evidence, not a proven universal tick size.

| Independently authored fixture | SHA-256 |
| --- | --- |
| baseline | `9519efd6ebad178fb1e1f012b7a967fbe8c0d6853987c405dcee7adfe6b57fb7` |
| double-tempo | `fbe037c97e197293411698d3da453a9df5adc35392ea35898fe53285798a29db` |
| short-gate | `dc30a1428a5352220040ae1b334169295f79af943f572f7ff9a7f271417f4bf9` |

Program/IPL/bootstrap hashes match the preceding pitch/selection milestones and
are printed alongside fixture hashes. Six runs are each bounded to 8000000 SNES
instructions and a 180-second child timeout. The expected instruction-bound exit
is mandatory. Trace files at the 32768-row cap or above 16 MiB fail the gate;
malformed/unordered DSP events, wrong source/voice/noise, missing or extra notes,
missing/nonpositive key-offs, wrong pitches and onset overlaps are rejected.

Raw trace CSVs can contain private RAM diagnostics. They and child stdout/stderr
remain temporary and are never forwarded. The report retains only case/control
metadata, hashes and relative timing intervals; no RAM, samples, absolute private
execution locations or PCM are exported. Failure exits 2 without partial JSON.
Success retains `qualification: false` and `playback: false`.

ROM-free tests check the complete eight-channel table, duration/control/note
stream, header checksums, transport boundaries, isolated operand changes,
key-on/off pairing, malformed/capped traces, matrix regressions, child-output
sanitization and CLI no-overwrite behavior. When the three private firmware files
exist under `roms/`, CMake registers a separate `local;private-reference` matrix.

```sh
ctest --test-dir build-dmg-firmware -R 'gameboy_sgb_(timing|pitch|song_selection)' --output-on-failure
```

Only duration 16, tempos 96/192, articulation `$7F`/`$3F`, instrument 2 and one
channel are covered. Other durations, tempo changes/fades, articulation values,
velocity, tie behavior, multi-channel phase, instrument tuning/sample ownership,
volume/pan and echo remain open. No physical-device or independent-emulator
comparison was performed. The prototype renderer is unchanged and continues to
reject vendor banks; commercial-title playback remains unsupported.
