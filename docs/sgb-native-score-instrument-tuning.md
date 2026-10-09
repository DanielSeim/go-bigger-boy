# Bounded exact-octave tuning

The opt-in D7 diagnostic extends [D6 score-ID mapping](sgb-native-score-instrument-mapping.md)
with one additional per-slot tuning selector. Selector 2 emits the thirteen
[instrument-10 pitch words measured on both originals](sgb-instrument-chromatic-reference.md).
Selectors 0 and 1 retain the preceding normal and full-word half-pitch meanings.
Qualification and production playback remain false; SGB1/SGB2 program ROMs
remain required. Performance optimization remains deferred.

## Build and admitted data

```sh
python3 scripts/build_sgb_score_transport.py --multisong --uploaded-instrument \
  --two-instruments --multiblock --instrument-profiles --one-shot --brr-profiles \
  --relocatable --atomic-upload --upload-recovery --instrument-mapping \
  --instrument-tuning --output /tmp/tuning-host.rom
python3 scripts/build_sgb_score_tuning_fixture.py --score-profile octave \
  --tuning 2,2 --output /tmp/tuning-game.gb
cmake --build build-dmg-firmware --target gameboy_sgb_score_tuning_probe
python3 tests/sgb_score_tuning_tests.py \
  --probe build-dmg-firmware/gameboy_sgb_score_tuning_probe
```

The flag is strictly boolean and requires D6 mapping and all its prerequisites.
The 256-KiB image SHA-256 is
`61cac6e3c1dbcfe40318714a6044bd68bc65170302e6c0fd3c64b0570efd8fdb`.
Its SPC payload is 6557 bytes (`$0200..1B9C`), leaving 99 bytes inside the
unchanged `$0200..1BFF` cap. The host is 1529 bytes (`$8000..85F8`). Exporters
refuse overwrite. D6 retains its exact preceding image hash; earlier opt-in
images and the bundled prototype are unchanged.

The 192-byte asset object and 2048-byte score bank retain their layout. Asset
offsets 18 and 22 (`$5012/$5016`) select tuning for physical slots 2 and 3:

| Selector | Pitch for admitted base notes 24..36 |
| --- | --- |
| 0 | Existing exact instrument-2 octave table |
| 1 | That table shifted right once as a complete 16-bit word |
| 2 | Exact instrument-10 octave table from the linked register reference |

Every other byte value rejects during complete asset admission, before any
song root or playback is published. The implementation uses an independently
entered parallel low/high table indexed by the already admitted `$98..A4`
opcode. It interpolates no pitches and accepts no uploaded tuning pointers or
arbitrary pitch words. Notes just outside the range remain invalid. Both selectors
are revalidated for each directory rehearsal and selection.

D6 score IDs at offsets 24/25 resolve to physical slots before descriptor and
tuning selection. ID 10 has no special runtime meaning. Each voice inherits its
own selected tuning through missing E0, patterns and controls. Ordered E0 prefixes
select the last executed slot; executed end selections update tuning, while
clipped tails do not. Fresh playback defaults both voices to slot 2 and its
uploaded selector. STOP, replacement and rejection recovery retain their prior
contracts. D7 identifies itself consistently in bridge, host and recovery paths.

This is tuning-only support. Physical SRCNs remain 2/3, the samples are owned,
and the envelope allowlist is unchanged. Original instrument-10 ADSR `$8E/$AF`
remains unsupported. Selecting tuning 2 does not reproduce that instrument's
sample, envelope or audible fundamental frequency. Matching DSP pitch words
makes no acoustic-equivalence claim.

## Validation

The dedicated tuning build of the existing whole-host probe records actual DSP
pitch registers ten ticks into each authored 16-tick note. It records all thirteen
notes on both voices, adds snapshot checkpoints as octave observations become
available, and includes the observations in exact full-state/PCM reset and
save/load comparison. The ordinary probe build has no added fields or behavior.
No emulator-core or production-selection changes are needed.

The tuning fixture sends newly authored octave or preceding multi-song streams
through real JOYP/SOU_TRN transport. Its bounds remain three physical payloads,
eight commands and one 4096-byte transfer frame per payload. Fixed sample windows,
BRR validation, atomic object clearing and recoverable rejection remain in force.

The public matrix covers:

- All thirteen words on each physical slot and voice, mixed normal/half/new
  selectors, reversed maps and zero/255 or opcode-valued score IDs. Both original
  model configurations execute the owned D7 image. Native/scalar PCM matches
  exactly within D7; combined output retains the bounded frame count.
- Voice-local inheritance, defaults, repeated and maximum E0 prefixes, executed
  end controls and clipped tails; observed first and final pitch/tuning metadata
  must match independent expectations.
- Active replacement changes both maps and selectors, with and without STOP;
  reordered selections and repeated physical uploads retain valid new objects.
- Invalid selectors 3/255 on each slot, neighboring reserved data and out-of-range
  notes reject silently. Fragmented cold and active repeated semantic failures
  recover with fresh complete objects, including final one-byte upload tokens
  and partial song admission revoked after a third-song failure.
- Frozen D6 rejects selector 2 and still renders normal/half profiles. Its actual
  pitch, descriptor and normalized-cache observations agree with D7's preceding
  modes. Different driver sizes shift upload/startup clocks, so whole-run PCM
  hashes are not compared across D6 and D7.

Every physical scenario requires exact reset and snapshot replay of full state
and PCM. The existing probe caps remain: 140 million master clocks, 4096 snapshots,
250000 PCM frames, 4096 output bytes and a 90-second child timeout. Octave runs
use 70 million clocks; replacement/recovery runs use at most 100 million;
reordered native reuploads use 125 million. No private ROM, sample, descriptor
or PCM is an implementation input. The reference pitch words come from the
preceding register-only measurement milestone; fresh private execution is not
needed to derive this finite table again. Hardware, independent-emulator,
acoustic and title qualification remain outstanding.

All eight final test methods pass across focused runs, covering 58 physical
model/mode/scenario cases. The octave/render group passes in 180.57 seconds;
inheritance, rejection and recovery pass in the initial lifecycle group; the
corrected replacement and D6-control group passes in 145.00 seconds. Two test
expectations were corrected: song 2 ends with voice-2 note 32, and different
image sizes do not imply equal upload/startup PCM timing. Firmware bytes and
measured pitch expectations did not change during these corrections. Unchanged
passing methods were not rerun after those assertion corrections.

All eight selected fixture, native chromatic/short-pair, original-transfer,
host and firmware build/reproducibility CTests pass (110.47 seconds). Twelve
build/cap/reproducibility checks spanning earlier transport images through D7
pass (7.24 seconds). The bundled prototype hash check and `git diff --check`
pass. Earlier long physical matrices were not rerun; their images remain frozen,
and the dedicated probe checks live D6 rejection and normal/half controls.

## Next step

Measure instrument-10 envelope setup and bounded ENVX evolution with owned note
fixtures on both original models. Its observed `$8E/$AF` descriptor lies outside
the current allowlist. Use register-only results to define a bounded envelope
extension, keeping sample/acoustic equivalence separate. Do not broaden envelope
admission merely to accept those bytes without checking attack/decay/sustain,
key-off, reset and save/load behavior.
