# Bounded uploaded envelope and tuning profiles

The opt-in D0 diagnostic image extends the [multi-block BRR profile](sgb-native-score-brr-chain.md)
with uploaded ADSR/direct-GAIN settings and per-instrument pitch scaling. Instruments
2 and 3 retain independent settings on both voices, including ordered E0 changes,
sparse-pattern inheritance, executed end controls and clipped tails.

Qualification and production playback remain false. SGB1/SGB2 program ROMs remain
required. This is an owned finite descriptor format, not a vendor instrument map
or a claim of original envelope, tuning or acoustic equivalence. Performance
optimization remains deferred.

## Build and admitted data

```sh
python3 scripts/build_sgb_score_transport.py --multisong --uploaded-instrument \
  --two-instruments --multiblock --instrument-profiles --output /tmp/profiles-host.rom
python3 scripts/build_sgb_score_instrument_profiles_fixture.py --profile distinct \
  --score-profile switch --output /tmp/profiles-game.gb
cmake --build build-dmg-firmware --target gameboy_sgb_score_transport_probe
python3 tests/sgb_score_instrument_profiles_tests.py \
  --probe build-dmg-firmware/gameboy_sgb_score_transport_probe
```

Exporters refuse overwrite. The image SHA-256 is
`951fbea38552e111403dd919f0892e37f75b0607cae95ce73731d93595e7c4e1`.
All preceding CB/CC/CD/CE/CF images and the bundled prototype retain their hashes.
All four preceding profile flags are required. The existing `$0200..1BFF`
combined-code cap, `$0800..17FF` native region, 4096-byte physical SOU_TRN payload,
2048-byte score bank, 192-byte uploaded asset object and ordered E0 cache remain
in use. No production emulator path changes.

| Object offset | Admitted data |
| --- | --- |
| 0, 4 | Fixed source IDs 2, 3 |
| 1..3, 5..7 | Per-source ADSR1, ADSR2, GAIN descriptor |
| 8..17 | Preceding fixed start/validated loop pointers and block counts |
| 18, 22 | Per-source tuning: 0 normal, 1 half pitch |
| 19..21, 23..63 | Zero reserved bytes |
| 64..191 | Preceding two bounded BRR windows |

An ADSR descriptor admits ADSR1 `8F`, `8A` or `FF`, ADSR2 `6F` or `4C`, and
fixed GAIN `B8`. These independently selected settings cover fast/slow attack
and fast/slow decay with two sustain profiles. ADSR remains enabled in this
mode, so DSP GAIN is ignored. A direct-GAIN descriptor instead requires ADSR1
and ADSR2 both zero and GAIN `40` or `60`. Dynamic GAIN modes, arbitrary ADSR
fields and arbitrary tuning values remain unsupported.

The uploaded tuning flag scales the existing thirteen-note pitch table. Normal
pitch is unchanged; half pitch shifts the full 16-bit word right, carrying the
high-byte low bit into the low byte. This is bounded owned transposition, not a
reconstructed vendor tuning table. Notes, control counts and execution budgets
retain the preceding guards.

Both descriptors and tuning flags are validated before each native rehearsal,
including all three silent directory admissions and every song selection.
Invalid data rejects the entire object before readiness or audio. Both voices
start each fresh score with the uploaded instrument-2 descriptor and tuning.
Each executed E0 writes the uploaded descriptor and updates that voice's tuning
in EC/ED. A missing E0 inherits both settings; an executed end E0 updates them;
an unexecuted clipped tail does neither. Validation and runtime scratch remain
separate. Key-off release uses the DSP's existing release behavior; the upload
does not introduce a programmable release parameter.

## Public validation

Six tests cover 98 physical model/mode/scenario runs, each with uninterrupted,
restored and cold-reset execution:

- Silent admission of both uploaded profiles on SGB1/SGB2.
- Eight owned descriptor profiles: baseline, changed envelope, changed tuning,
  distinct settings, swapped settings, direct GAIN, changed decay and crossed ADSR fields.
  Tick-10 DSP observations before the first note's gate end verify each voice's
  actual pitch and ENVX. Envelope-only and tuning-only comparisons isolate their
  effects. Direct GAIN produces observed ENVX values 64 and 96. The completed
  score must leave both ENVX values zero and a silent tail of at least one million
  master clocks. Native/scalar PCM matches exactly; combined resampling retains
  bounded frame counts and the same envelope/pitch observations.
- Voice-local inheritance, ordered repeated prefixes, executed end selection,
  clipped tails, both sources playing on both voices and first notes with no E0
  using the uploaded instrument-2 defaults. Final DSP descriptors
  and voice-local tuning must agree with the independently expected selections.
- Active selection in all three modes, stop/resume and repeated physical upload
  on both models, with positive ENVX at the bounded interruption times.
- Twenty-six malformed cases per model: wrong source IDs, disabled or unsupported
  ADSR combinations, unsupported GAIN, invalid direct-GAIN combinations, tuning
  2/255, nonzero reserved data and a missing object. These must reject silently
  before any directory root is admitted. Six of these cases recheck the inherited
  BRR count, loop, termination, filter and padding guards in the D0 image.

The probe includes observed first-note pitch/envelope, final descriptors,
voice-local tuning, final pitch and final ENVX in exact reset/restoration comparison, along
with full serialized state, PCM and the preceding source/sample/cache metadata.
All thirteen selected envelope/tuning, sample, instrument, directory, transport,
host, transfer, firmware, fixture and native-engine CTest suites pass. The prior
profiles retain their 332 physical model/mode/scenario checks, including 104 BRR
chain cases. CLI byte reproducibility and overwrite protection, the bundled
prototype hash check and `git diff --check` also pass.

No proprietary firmware, descriptor, tuning data, bank or reference PCM is used.
Original-decoder, hardware, acoustic and title qualification remain outstanding.
Partial uploads still lack atomic asset invalidation.

## Next step

The [D1 diagnostic](sgb-native-score-one-shot.md) adds bounded non-looping BRR
samples and natural-completion/gate/lifecycle evidence. The [D2 profile](sgb-native-score-brr-profiles.md) adds bounded BRR filters/ranges.
Next add bounded relocatable sample-directory entries while retaining prior image
contracts and production qualification boundaries.
