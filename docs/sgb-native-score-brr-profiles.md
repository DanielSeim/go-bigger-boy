# Bounded BRR filter and range profiles

The opt-in D2 diagnostic extends the [one-shot profile](sgb-native-score-one-shot.md)
with all four BRR predictors and two admitted ranges. Each declared block may
choose filter 0..3 and range 8 or 11 independently. Intermediate and terminal
flags retain the preceding looping/one-shot contract.

Qualification and production playback remain false. SGB1/SGB2 program ROMs remain
required. This remains a finite owned format: two instrument IDs, fixed sample
windows, one to four blocks per source and the preceding bounded score subset.
Other ranges, arbitrary sample directories, larger banks, vendor instrument
maps, echo and broader scheduling remain unfinished. Performance optimization
remains deferred.

## Build and admission

```sh
python3 scripts/build_sgb_score_transport.py --multisong --uploaded-instrument \
  --two-instruments --multiblock --instrument-profiles --one-shot --brr-profiles \
  --output /tmp/brr-profiles-host.rom
python3 scripts/build_sgb_score_brr_profiles_fixture.py --brr-profile mixed \
  --sample-profile loop --output /tmp/brr-profiles-game.gb
cmake --build build-dmg-firmware --target gameboy_sgb_score_transport_probe
cmake --build build-dmg-firmware --target gameboy_sgb_score_brr_decode_probe
python3 tests/sgb_score_brr_profiles_tests.py \
  --probe build-dmg-firmware/gameboy_sgb_score_transport_probe \
  --decode-probe build-dmg-firmware/gameboy_sgb_score_brr_decode_probe
```

The image SHA-256 is
`a71cc1cec4e6bbc2c7220f144c601b65f20049683e2385c341156bd709cae1b2`.
All six preceding flags are required. Exporters refuse overwrite. Previous
CB/CC/CD/CE/CF/D0/D1 images and the bundled prototype retain their hashes.
The code cap remains `$0200..1BFF`, native region `$0800..17FF`, uploaded bank
2048 bytes, asset object 192 bytes and physical SOU_TRN payload 4096 bytes.
No production emulator path changes or probe RAM injection are introduced.

For every declared header, admission first requires its high nibble to be
8 or 11. All values of its two filter bits are admitted. The helper then checks
only the two flag bits: 0 before the last block, 3 for a looping terminal block,
and 1 for a one-shot terminal block. A loop-only flag or an early end rejects.
This accepts per-block predictor/range changes without relaxing end boundaries.

Fixed source IDs, start addresses, block-count bounds, loop alignment/window
checks, canonical one-shot loop words, mode flags, zero padding/reserved bytes,
and envelope/tuning checks remain in use. Both sources pass admission before
any directory root is published and before every subsequent selection. Range
12 is intentionally outside this finite profile, despite the decoder supporting
it. Unsupported data rejects silently before readiness.

## Arithmetic evidence

A bounded standalone decoder probe reads the owned 192-byte object and decodes
both sources from zero history, then once more from each declared loop header
without clearing prediction history. The second traversal is an arithmetic
check even for one-shot objects; it does not claim that their second traversal
is audible. Counts, pointer alignment and traversal lengths are bounded.

A separate Python oracle uses exact rational predictor contributions, rounding
each contribution down before addition. Its coefficient pairs are `(0,0)`,
`(15/16,0)`, `(61/32,-15/16)` and `(115/64,-13/16)`. It applies explicit signed
nibbles, residual scaling, 16-bit saturation and signed 15-bit wrapping.
This formulation differs from the decoder's integer-shift implementation.

Fixed hand-calculated range-8 impulse vectors anchor the oracle's initial
samples, including asymmetric negative rounding:

| Filter | Positive impulse, first three samples | Negative impulse, first three samples |
| --- | --- | --- |
| 0 | 128, 0, 0 | -128, 0, 0 |
| 1 | 128, 120, 112 | -128, -120, -113 |
| 2 | 128, 244, 345 | -128, -244, -346 |
| 3 | 128, 230, 309 | -128, -230, -310 |

Sixty owned objects cover all eight fixed filter/range combinations, mixed and
swapped per-block profiles, counts 1..4, loop and one-shot layouts, block
boundaries and loop re-entry. The complete decoded sequences must match the
oracle. The existing decoder contract also covers reset/history and overflow.
This is an independent arithmetic cross-check, not an original-decoder or
hardware reference comparison.

## Physical evidence

Eight tests cover 148 physical model/mode/scenario runs, each with uninterrupted,
restored and cold-reset execution:

- Silent directory admission on both models.
- All eight fixed combinations plus mixed/swapped block profiles in looping
  sources on SGB1/SGB2. All ten produce distinct PCM digests within each model.
  Filter 3/range 11 and mixed/swapped profiles also have scalar/combined checks.
- All eight combinations with one-shot sources; mixed looping/one-shot sources
  in native, scalar and combined modes. Natural completion must show zero ENVX,
  latched ENDX, positive native gate counters and clear KOF simultaneously;
  looping sources retain positive ENVX at the same settled note observations.
- Uploaded ADSR/tuning and direct GAIN, ordered repeated E0, sparse inheritance,
  executed end selection, clipped tails, both instruments on both voices and
  initial notes without E0 while predictive samples play.
- Live looping-source selection in all modes, stop/resume and physical reupload
  on both models, preserving positive ENVX at bounded interruption times.
- Twenty-nine malformed cases per model: unsupported ranges on both sources,
  early/missing ends and loop-only flags, invalid mode/loop/count/padding,
  envelope/tuning/reserved data and an all-zero missing object. These must
  reject silently before admitting any directory root.

The read-only host probe records all eight uploaded header positions alongside
source modes/counts/loops, pitch/envelope/gate/end observations, prefix caches,
PCM and full serialized state. Native/scalar PCM matches exactly; combined
resampling retains bounded frame counts. Independent schedules remain
72/72/76 ticks and completed scores retain a silent tail. ENDX/envelope-zero
snapshot phases and the existing execution/snapshot/audio caps remain in use.

All nineteen selected BRR-profile, sample, instrument, directory, transport,
host, transfer, firmware, fixture, native-engine and BRR/DSP contract CTest suites
pass. The preceding profiles retain 550 physical model/mode/scenario checks
with the expanded probe. CLI byte reproducibility and overwrite protection,
prior image hashes, the bundled prototype check and `git diff --check` also pass.

No proprietary firmware, bank, sample, descriptor or reference PCM is used.
Hardware, original-decoder, acoustic and title qualification remain outstanding.
Partial uploads still lack atomic asset invalidation.

## Next step

The [D3 relocation profile](sgb-native-score-relocated.md) now admits bounded
relocatable starts/loops within the uploaded data windows, with complete-chain
bounds, zero prefix/suffix padding and physical lifecycle replay. The [D4 atomic-upload profile](sgb-native-score-atomic-upload.md) now prevents
stale score/sample reuse. Next add bounded recovery after upload rejection. Broader vendor banks and production qualification remain separate.
