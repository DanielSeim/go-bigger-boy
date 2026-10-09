# Bounded relocatable sample directories

The opt-in D3 diagnostic extends the [D2 BRR profile](sgb-native-score-brr-profiles.md)
with relocatable start and loop words inside the existing uploaded sample windows.
It keeps two source IDs, counts 1..4, ranges 8/11, filters 0..3, looping/one-shot
modes and the preceding envelope, tuning and bounded score contracts.

Qualification and production playback remain false. SGB1/SGB2 program ROMs are
still required. This diagnostic does not qualify vendor banks, hardware, original
firmware playback or real titles. Performance optimization remains deferred.

## Build

```sh
python3 scripts/build_sgb_score_transport.py --multisong --uploaded-instrument \
  --two-instruments --multiblock --instrument-profiles --one-shot --brr-profiles \
  --relocatable --output /tmp/relocated-host.rom
python3 scripts/build_sgb_score_relocated_fixture.py --layout last \
  --sample-profile mixed3 --output /tmp/relocated-game.gb
cmake --build build-dmg-firmware --target gameboy_sgb_score_transport_probe \
  gameboy_sgb_score_brr_decode_probe
python3 tests/sgb_score_relocated_tests.py \
  --probe build-dmg-firmware/gameboy_sgb_score_transport_probe \
  --decode-probe build-dmg-firmware/gameboy_sgb_score_brr_decode_probe
```

The image SHA-256 is
`771bf03cfc3c1c091488fea1c0d26b9f78847edb970c9a7a44dab636d7618600`.
All seven preceding flags are required. Exporters refuse overwrite. Earlier
CB/CC/CD/CE/CF/D0/D1/D2 images and the bundled prototype retain their hashes.
The code cap remains `$0200..1BFF`, native region `$0800..17FF`, score bank 2048
bytes, asset object 192 bytes and physical SOU_TRN payload 4096 bytes.
No production emulator path changes or probe RAM injection are introduced.

## Admission

| Source | Start/loop words | Fixed data window | Allowed start |
| --- | --- | --- | --- |
| 2 | `$5008/$500A` | `$5040..507F` | `$5040 + 9*k` |
| 3 | `$500C/$500E` | `$5080..50BF` | `$5080 + 9*k` |

For count `n`, `0 <= k <= floor((64-9*n)/9)`. Alignment is relative to the window
base, not to absolute address zero. The start must belong to its own fixed
window and every declared nine-byte block must fit completely in it. Directory
high bytes must remain `$50`. The fixed window end is never derived from the
uploaded start. Pointers into metadata, code, score, prefix cache, the other
source or outside the object reject before publishing any root.

A loop word must name one of the declared headers relative to the relocated
start. One-shot objects require loop = start. All bytes before the chain and
after it, up to the fixed window bounds, must be zero. Existing header flag,
range, mode, reserved, envelope and tuning guards remain active. Both sources
are checked during silent directory admission and before every selection.
DSP DIR stays `$50`; the DSP reads the uploaded directory directly. No sample
copy, larger allocation or additional source ID is involved.

## Validation

The decoder probe now checks the uploaded start, relative alignment, complete
chain bounds and loop membership before decoding. The independent rational
oracle covers 220 objects: every allowed start/count pair, both sources moving
independently, all eight fixed range/filter combinations and mixed/swapped
profiles. Another 150 objects cover relocated loop re-entry and mixed source
modes across all five owned layouts. Decoded sequences must match exactly.
The one-shot second traversal remains an arithmetic check, not audible playback.

Eight tests cover 192 physical model/mode/scenario runs, each with uninterrupted,
restored and cold-reset execution:

- Silent admission on SGB1/SGB2 with starts at their last valid positions.
- Eight count/mode layouts across base, near, last, source-2-only and
  source-3-only relocation, on both models. Both sources play on both voices;
  ENDX, ENVX, gate counters, KOF and silent tails retain their earlier contracts.
- Native/scalar exact PCM parity and bounded combined-mode frame counts, plus
  ordered/repeated E0, inherited instruments/tuning, executed end controls,
  clipped tails and initial notes without E0.
- Active selection in all render modes, stop/resume and physical reupload,
  preserving positive ENVX at bounded looping-source interruption times.
- Forty malformed objects on each model: out-of-window, cross-source,
  unaligned and overlapping start/loop pointers, overflowing complete chains,
  nonzero prefix/suffix padding, invalid counts/modes, early ends and
  noncanonical one-shot loops. These reject silently before admitting roots.

The read-only host probe adds both uploaded start words to its observations and
full replay comparisons. Existing fixed header observations retain their
meaning. A relocated layout can change validation timing and therefore the
absolute PCM digest relative to the base layout. Layout comparisons require
identical decoded samples, identical output frame counts, a difference of at
most 16 nonzero frames and a final-audio-clock difference of at most 50000
master clocks. Uninterrupted/restored/reset and native/scalar comparisons for
the same layout still require exact PCM equality. Schedules remain 72/72/76 ticks.
Child runtime, master clocks, snapshots, output and PCM sizes remain bounded.

All twenty selected relocation, BRR-profile, one-shot, instrument, directory,
transport, host, transfer, firmware, fixture, native-engine and BRR/DSP contract
CTest suites pass (1416.48 seconds total). The relocation suite passes in
1055.34 seconds. The preceding profiles retain 698 physical model/mode/scenario
checks with the expanded probe. CLI byte reproducibility and overwrite
protection, prior image hashes, the bundled prototype check and
`git diff --check` also pass.
No proprietary firmware, bank, sample, descriptor or reference PCM is used.
Original-decoder, hardware, acoustic and title qualification remain outstanding.

## Next step

The [D4 atomic-upload profile](sgb-native-score-atomic-upload.md) now preflights
complete score/sample coverage, clears both regions before IPL and publishes
readiness only after all roots validate. Next add bounded recovery after upload
rejection, keeping SOUND unavailable until a later complete generation validates.
Broader sample banks and title qualification remain separate.
