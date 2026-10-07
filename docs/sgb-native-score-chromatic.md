# Experimental measured chromatic octave

The isolated chromatic renderer extends
[gated finite-call playback](sgb-native-score-callgate.md) from three notes to
all base notes 24..36 inclusive (`$98..A4`). It remains an experimental native
SPC driver, outside bundling and production selection. SGB1/SGB2 program ROMs
are still required.

## Black-box pitch measurements

The owned `build_sgb_chromatic_fixture.py` cartridge sends a newly authored
ascending octave through actual JOYP/SOU_TRN transport. Its raw bank has two
patterns, channels 2/3, instrument 2, pan 10, track volume 127, song volume 160
and tempo 96. Both voices play the same notes, with duration 16. The first
pattern has seven notes and the second has six. No instrument table, sample,
private score or original firmware instructions are used as implementation
inputs.

Initial private SGB1/SGB2 runs produced identical words for all thirteen notes,
including the previously measured anchors 24, 25 and 36. The observer now pins
every value exactly, rather than accepting approximate semitone ratios.

| Base note | Opcode | Measured instrument-2 DSP pitch |
| --- | --- | --- |
| 24 | `$98` | 1068 |
| 25 | `$99` | 1132 |
| 26 | `$9A` | 1200 |
| 27 | `$9B` | 1272 |
| 28 | `$9C` | 1348 |
| 29 | `$9D` | 1428 |
| 30 | `$9E` | 1512 |
| 31 | `$9F` | 1604 |
| 32 | `$A0` | 1700 |
| 33 | `$A1` | 1800 |
| 34 | `$A2` | 1908 |
| 35 | `$A3` | 2020 |
| 36 | `$A4` | 2140 |

Both original voices must use SRCN 2, ADSR1 `$8F`, ADSR2 `$6F`, GAIN `$B8`
and no noise at every nonzero KON. These are register observations for one
instrument and octave. They do not establish a universal tuning algorithm,
other instrument mappings, transpose/fine tuning or samples' fundamental
frequencies. Instrument-10 observations remain limited to the prior three
notes.

## Native rendering

The independent writer validates the entire note range before playback and
uses parallel low/high-byte tables at `$0F40/$0F50`. Validated opcodes index
the tables directly; there is no runtime pitch interpolation. The gate profiles
remain at `$0F80`. The existing strict assembler rejects overlapping sections.
The reproducible 2073-byte artifact has SHA-256
`d9bec07d99181ca2232515a181f6e3fc7693eab6bdaf47a1d157c2d151b5b783`.
The prior gated-call and other driver artifacts remain unchanged.

The native timer starts after full bank validation, silent timeline rehearsal
and DSP setup, before preparing the initial notes. This bounds the first-note
startup interval without adding initial note preparation after KON to the
start of the timer clock. Subsequent events retain the same pulse/tick clock.
All ten measured gate profiles retain their original pulse counts and timing
allowances. Nothing adjusts reported timestamps; the probe observes actual
native KON/KOF writes.

The existing bounds remain: 1..128 raw bank bytes, exactly two patterns,
channels 2/3, 1..8 expanded events per track, one nonnested finite call per
track with counts 1..3, and 32 events/2032 ticks overall. Duration/articulation
inherit through repeats and returns. Articulations 63/127, rests, independent
voice gates, first-end clipping and complete silent rejection retain their
prior contracts.

DSP source 0, authored square-wave BRR, direct gain 127 and separate left/right
routing remain diagnostic. Matching instrument-2 pitch words does not reproduce
the original waveform, envelopes, instrument controls or PCM. Installation is
still an owned IPL trampoline/direct RAM diagnostic; whole-system upload and
production integration remain unqualified.

```sh
python3 scripts/build_sgb_score_chromatic.py --output /tmp/score-chromatic.bin
cmake --build build-dmg-firmware --target gameboy_sgb_score_chromatic_probe
ctest --test-dir build-dmg-firmware -R 'native_score_chromatic$' --output-on-failure
```

The nine-case component suite checks both fixture shapes and articulations,
the full octave through every measured gate profile, all ten new pitches
through three-repeat calls and returns, inherited/changing articulation and
duration, asynchronous voices, surviving peer PCM and clipped notes. It checks
unsupported notes immediately outside the octave, ties, unsupported late
profiles and every truncated fixture prefix. Full reset and cross-instance
save/load preserve events, pitch snapshots, actual key edges, gate selection,
completion time and owned PCM fingerprints. Each release and event pair has
a continuation checkpoint.
All fifteen native score suites and both phrase/subroutine fixture suites
passed; the chromatic suite also passed after the timer-startup change.

## Coupled original-reference validation

```sh
python3 scripts/check_sgb_chromatic_reference.py \
  --trace build-dmg-firmware/gameboy_snes_65c816_apu_trace --firmware-dir roms
python3 scripts/check_sgb_score_chromatic_reference.py \
  --probe build-dmg-firmware/gameboy_sgb_score_chromatic_probe \
  --trace build-dmg-firmware/gameboy_snes_65c816_apu_trace --firmware-dir roms
```

The combined matrix compares both SGB models, both articulations and two owned
fixtures. The ascending case has 13 onsets and completes at tick 208. The call
case repeats a shared three-note body `(24,28,32)` twice, returns to caller
notes `(25,29)`, then advances to `(26,27,30,31,33,34,35,36)` in the next
pattern. It has 16 onsets, completes at tick 256, and also covers all thirteen
notes. Caller continuation inherits the body's duration and articulation.

| Case | Articulation | Bank bytes | Owned cartridge SHA-256 |
| --- | --- | --- | --- |
| chromatic | 127 | 118 | `28689aa06c2e81aed0750e33648181e854082383ab29dedc958b6aa30e669c40` |
| chromatic | 63 | 118 | `b38f7d3bc66a89f891f1acc330b0d5e20bd740c78ae237edf1cc50b3f8fc156b` |
| calls | 127 | 122 | `3ff6d53043426151138229fa340c75c94bee81e3d9463020541b12b2aaa94ba2` |
| calls | 63 | 122 | `c5bd7f878aef3f9b256ccc3a95cc5a52bfe053ffc95c2349ee78c48e61b94350` |

The validator requires exact original pitch words, source/envelope setup,
combined KON masks and repeat/return/pattern sequences. Each original note
must have a directly observed KOF release; missing releases cannot be inferred
from retriggers or pattern handoffs. Original adjacent onset intervals retain
the existing 84000..92000-cycle fixture bounds and two-model agreement within
2048 cycles. Actual native DSP onset spacing and per-voice gate lengths must
match within the existing 4096-cycle allowance.

All eight fresh comparison runs passed, with 232 directly observed per-voice
KOF releases. Maximum differences were 2953 SPC cycles for native/original
DSP onset spacing, 3558 for gates and 472 between original model onset
intervals. No gate count, pitch word or timing allowance was relaxed.

Private runs retain the 8000000-instruction bound, 180-second child timeout,
16-MiB trace bound and fewer than 32768 trace rows. Raw traces and child output
remain temporary; only sanitized register/timing metadata and hashes are
exported. No original PCM, physical-device or independent-emulator comparison
was performed. Fresh coupled checks cover tempo 96/duration 16; other measured
gate profiles and asynchronous behavior have separate component evidence.

Schemas are `gbb-sgb-chromatic-reference-v1`, `gbb-spc-score-chromatic-v1` and
`gbb-score-chromatic-reference-v1`, all with qualification and playback false.
Broader pitch/instrument coverage, controls, phrase/channel grammar and
production integration remain ahead. The bundled prototype is unchanged.
