# Register-only instrument-10 octave reference

The owned chromatic fixture now measures base notes 24..36 (`$98..A4`) with
resident instrument 10 on voices 2 and 3 of both private original models.
Instrument 2 is retained as a fresh control. These are bounded black-box DSP
register observations, not a replacement instrument implementation. Qualification
and production playback remain false; SGB1/SGB2 program ROMs remain required.

## Owned fixture and retained observations

`build_sgb_chromatic_fixture.py --instrument 10` changes only the two authored
E0 operands in the existing ascending fixture. Its 118-byte score has two
patterns, seven then six notes, duration 16, articulation 127, pan 10, track
volume 127, song volume 160 and tempo 96. Both channels inherit the instrument
into the second pattern. Default instrument-2 images, including both call
fixtures and articulations, retain their preceding hashes.

The instrument-10 ascending cartridge SHA-256 is
`756cd7a43fbf660f7d0ec94ab28a35bfed3f55216e66d7ae1bd0dab73dcd7581`.

The new checker uses only the ascending, articulation-127 case. It requires
exactly thirteen nonzero combined KONs (`$0C`), pitched voices with NON clear,
complete DSP pitch/source/envelope setup on both voices and agreement with the
pinned words below. Both originals agree exactly for both voices at every note.

| Base note | Instrument 2 pitch | Instrument 10 pitch |
| --- | --- | --- |
| 24 | 1068 | 7993 |
| 25 | 1132 | 8472 |
| 26 | 1200 | 8981 |
| 27 | 1272 | 9520 |
| 28 | 1348 | 10088 |
| 29 | 1428 | 10687 |
| 30 | 1512 | 11316 |
| 31 | 1604 | 12004 |
| 32 | 1700 | 12723 |
| 33 | 1800 | 13471 |
| 34 | 1908 | 14280 |
| 35 | 2020 | 15118 |
| 36 | 2140 | 16016 |

Instrument 2 requires SRCN 2, ADSR1 `$8F`, ADSR2 `$6F`, GAIN `$B8`.
Instrument 10 requires SRCN 10, ADSR1 `$8E`, ADSR2 `$AF`, GAIN `$B8`.
The prior instrument-10 single-voice notes 24, 25 and 36 remain exact anchors.
No source directory, instrument-table bytes, BRR samples, original instructions,
PCM or private score are retained or used as implementation inputs.

```sh
python3 scripts/build_sgb_chromatic_fixture.py --instrument 10 \
  --output /tmp/instrument-10-chromatic.gb
python3 scripts/check_sgb_instrument_chromatic_reference.py \
  --trace build-dmg-firmware/gameboy_snes_65c816_apu_trace --firmware-dir roms
ctest --test-dir build-dmg-firmware --output-on-failure \
  -R 'sgb_instrument_chromatic_(fixture|local_reference)$'
```

Private reference execution retains the existing 8000000-instruction bound,
180-second child timeout, 16-MiB trace cap and fewer than 32768 trace rows.
Only instruction-bound completion is accepted. Child output and raw traces
remain temporary; exported JSON contains register setup and input hashes only,
under schema `gbb-sgb-instrument-chromatic-reference-v1`, qualification/playback
false. The CMake private test is enabled only when all original firmware inputs
are present. The public parser/fixture test needs no private inputs.

This checker deliberately makes no gate, onset-timing, envelope trajectory or
sample/acoustic equivalence claim. Existing instrument-2 gate/timing qualification
has its separate [chromatic evidence](sgb-native-score-chromatic.md). Register
pitch alone does not establish a sample's audible fundamental frequency, a
universal multiplication factor, interpolation, transpose or fine tuning.
Notes outside 24..36 and other resident instruments remain unmeasured here.

## Next bounded tuning contract

The [D7 tuning diagnostic](sgb-native-score-instrument-tuning.md) implements
this bounded contract as an opt-in successor to
[D6 score-ID mapping](sgb-native-score-instrument-mapping.md), keeping previous
artifact hashes and production defaults unchanged:

- Preserve descriptor tuning selector 0 (the existing instrument-2 octave) and
  selector 1 (its existing full-word right shift). Admit selector 2 for the exact
  thirteen instrument-10 pitch words above; reject all other selector values.
  Reuse the current per-slot tuning bytes at asset offsets 18 and 22. Do not
  introduce arbitrary uploaded pitch tables or infer a general tuning formula.
- Keep tuning attached to physical owned slots 2/3. Score IDs still resolve
  through the D6 map; ID 10 alone must not change pitch. Selecting an E0 updates
  that voice's mapped descriptor/tuning; missing E0 inherits them. Fresh playback
  defaults to slot 2, including its tuning selector.
- Validate both tuning selectors during complete asset/directory admission.
  Unknown selectors reject before readiness/audio; D5 recovery and D4 atomic
  generation replacement still apply. The preceding D6 image must continue to
  reject selector 2. Preserve fixed sample bounds and the `$0200..1BFF` code cap.
- Exercise all thirteen notes with selector 2 on each physical slot and voice,
  mixed normal/half/new profiles, reversed score maps, inheritance, ordered
  prefixes, executed end controls and clipped tails. Check actual DSP pitch
  writes, silent rejection, active replacement, STOP/reupload, reset and exact
  save/load replay on both models. Use owned samples for PCM continuity tests.

This contract extends tuning only. In particular, the current owned envelope
allowlist does not admit instrument-10 ADSR `$8E/$AF`, and owned slots still use
their own samples/SRCNs. Adding selector 2 must not be described as implementing
resident instrument 10 or reproducing its timbre. Envelope and acoustic work
remain separate milestones. Performance optimization remains deferred.

## Validation evidence

Initial instrument-10 discovery on both private originals agreed for every
word. A subsequent pinned four-run checker pass covered both instruments and
models: 52 combined KONs and 104 per-voice setup observations. No tolerances
were substituted for exact pitch/setup equality. Raw traces were discarded.

The four public fixture/parser tests pass, covering frozen instrument-2 hashes,
owned instrument-10 transport, exact model/instrument coverage, missing/extra
KONs, wrong voices, noise, incomplete setup, invalid/unordered DSP rows, the row
bound, changed measured words and exclusion of private diagnostic rows. Both
new CMake tests are registered. All eight selected CTests pass (74.71 seconds):
the new fixture/parser, preceding pitch fixture, native chromatic/short-pair,
original transfer, host, firmware build and reproducibility contracts. The
bundled prototype hash check and `git diff --check` also pass.
Physical-device, independent-emulator, title and acoustic checks were not run.
