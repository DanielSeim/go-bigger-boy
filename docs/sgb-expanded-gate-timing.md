# Controlled duration and tempo gate observations

The [native gate experiment](sgb-native-score-gate.md) currently supports three
explicit duration-16 profiles. The next owned fixture matrix measures additional
durations and tempos before changing native support. It uses independently
authored three-note score banks with notes 24/25/36 and fixed channel-2
instrument, pan and volume setup. Only tempo, duration and articulation vary.
No program code, samples or private PCM are copied into these fixtures.

| Case | Tempo | Articulation | Duration |
| --- | --- | --- | --- |
| control | 96 | `$7F` | 16 |
| half-duration | 96 | `$7F` | 8 |
| long-duration | 96 | `$7F` | 24 |
| half-duration-short | 96 | `$3F` | 8 |
| long-duration-short | 96 | `$3F` | 24 |
| middle-tempo | 128 | `$7F` | 16 |
| middle-tempo-short | 128 | `$3F` | 16 |
| double-tempo-short | 192 | `$3F` | 16 |

The control cartridge is byte-identical to the earlier baseline timing fixture.
All eight transfer payloads differ only at the three parameter byte positions;
GB header/global checksums are regenerated. Each song contains the same three
notes followed by a duration-1 rest and terminator. The separate symbolic oracle
checks duration, articulation and total score ticks for every owned bank.

## Commands

```sh
python3 scripts/build_sgb_gate_timing_fixture.py \
  --case half-duration --output /tmp/half-duration.gb
ctest --test-dir build-dmg-firmware \
  -R '^gameboy_sgb_(gate_timing|timing)_fixture$' --output-on-failure
python3 scripts/check_sgb_gate_timing_reference.py \
  --trace build-dmg-firmware/gameboy_snes_65c816_apu_trace --firmware-dir roms
```

Existing output files are preserved. The optional local reference test requires
caller-owned `sgb1.program.rom`, `sgb2.program.rom` and `spc700.rom`; public
checks use owned inputs and synthetic DSP traces only. Each of sixteen private
runs must end at the existing eight-million-instruction bound, with a 180-second
child timeout and bounded DSP trace bytes/rows. The shared observer requires
three complete channel-2 notes, the expected pitches/source and positive,
ordered key-on/key-off observations. Extra private child output and RAM/sample
rows are captured and discarded.

The matrix checks all expected model/case pairs and parameter metadata. It
compares duration-8/24 onset ratios with the control, tests tempo-128 and
short-articulation tempo-192 onset spacing, requires paired short articulation
to shorten every gate without changing spacing beyond one pulse, and compares
SGB1/SGB2 timings within 2048 SPC cycles. Per-case gate bounds pin the measured
ranges with bounded headroom. Missing, malformed or regressed
observations fail without a partial success report. These are bounded fixture
relationships, not an inferred general scheduling formula.

## Observed timing ranges

| Case | Onset interval range | Gate interval range |
| --- | --- | --- |
| control | 86166..88049 | 73779..74848 |
| half-duration | 42994..43454 | 29329..31694 |
| long-duration | 129171..131061 | 117391..119321 |
| half-duration-short | 42994..43454 | 18942..21431 |
| long-duration-short | 129169..131059 | 74377..76772 |
| middle-tempo | 63635..65518 | 53907..56219 |
| middle-tempo-short | 63638..65521 | 33419..35346 |
| double-tempo-short | 42997..43457 | 21130..23052 |

Duration-8 onset spacing is about half of duration-16 spacing, but its normal
gates are about 0.4 times the control gate, rather than 0.5. Duration-24 normal
gates are about 1.6 times the control, while onset spacing is about 1.5 times.
The ROM-free regression matrix explicitly rejects the naive half-duration gate
prediction of 37000 cycles. These observations constrain the next native
implementation without asserting a general interpolation rule.

Ranges include the two onset intervals and three note gates from both model
runs, in SPC cycles. Gate timing describes DSP key-off register writes rather
than the final audible release envelope. This matrix does not compare PCM or
qualify every value between the tested parameters.

Reports export sanitized interval metadata and input hashes and retain
`qualification: false` and `playback: false`. Native gate constants, supported
profiles, clock validation, bundled firmware, production ownership and external
image requirements are unchanged. The next step is to use this evidence to
validate additional explicitly bounded native gate profiles, with their own
reset/save-load and owned PCM checks.
