# Explicit score instrument IDs

The opt-in D6 diagnostic extends [D5 recovery](sgb-native-score-upload-recovery.md)
with an uploaded two-entry map from score instrument IDs to independently owned
sample slots. An E0 selector of 10 can now choose slot 3 without treating 10 as
a DSP source number or requiring score authors to rewrite it as 3.

This separates score identity from sample storage. It does not reproduce the
original instrument-10 sample, pitch or envelope. Qualification and production
playback remain false, SGB1/SGB2 program ROMs remain required, and performance
optimization remains deferred.

## Build

```sh
python3 scripts/build_sgb_score_transport.py --multisong --uploaded-instrument \
  --two-instruments --multiblock --instrument-profiles --one-shot --brr-profiles \
  --relocatable --atomic-upload --upload-recovery --instrument-mapping \
  --output /tmp/mapping-host.rom
python3 scripts/build_sgb_score_mapping_fixture.py --ids 2,10 \
  --output /tmp/mapping-game.gb
cmake --build build-dmg-firmware --target gameboy_sgb_score_transport_probe
python3 tests/sgb_score_mapping_tests.py \
  --probe build-dmg-firmware/gameboy_sgb_score_transport_probe
```

The 256 KiB image SHA-256 is
`6b2fdf4494e7715c6899335ebe34f39dcd62728cb6fd3a1d34f562d629ee9607`.
All preceding flags are required. The SPC payload is 6508 bytes, ending at
`$1B6B`, within `$0200..1BFF`, leaving 148 bytes. The host payload is 1529 bytes.
Earlier CB through D5 diagnostic images and the bundled prototype retain their
hashes. Exporters refuse overwrite. Native engine, cache, 2048-byte score,
192-byte asset object and 4096-byte SOU_TRN frame bounds are unchanged.

## Mapping contract

| Asset address | Meaning | Default fixture |
| --- | --- | --- |
| `$5018` | Score ID bound to owned slot 2 / descriptor `$5000` | 2 |
| `$5019` | Score ID bound to owned slot 3 / descriptor `$5004` | 10 |

Both IDs are bytes and must differ. Zero and 255 are valid explicit IDs; an
omitted map contains two zeros and rejects as ambiguous. Every other reserved
asset byte retains D3's zero requirement. Descriptor SRCN fields must still be
2 and 3, and start/loop pointers must still pass the existing per-sample window
and BRR-chain checks. The map cannot authorize other DSP slots or addresses.

During whole-bank admission, each E0 operand is looked up in this table.
Unknown IDs reject. A successful lookup becomes slot 2 or 3 in the existing
prefix cache. Rendering consumes these resolved slots and their bounded
source/envelope/tuning descriptors, preserving prefix order. The logical ID is
never written directly to SRCN, used as a pointer or used to index an arbitrary
instrument array. Omitted E0 retains the existing voice-local descriptor and
tuning; it does not mean instrument ID zero. Initial omitted E0 uses the existing
owned slot-2 default.

Mapping validation precedes descriptor/sample validation and song-root admission.
No readiness publishes until all three roots admit. Unknown selectors in song
3 therefore revoke partial admission of songs 1 and 2. D5's mute, block, token
synchronization and retry contracts apply unchanged with a D6 advertisement.
The next complete upload clears both objects, including the old mapping, before
validating a fresh generation. Sending only the two map bytes fails complete-
object preflight and cannot change a ready generation in place.

The physical descriptor/sample object remains deliberately small: two slots,
one to four BRR blocks in each 64-byte window, normal/half pitch, and the existing
bounded envelope choices. A score ID of 10 selects an owned descriptor; it does
not select a hidden resident vendor preset. The [instrument observations](sgb-pitch-instrument-fixtures.md)
measured different pitch and envelope setup for the original instrument 10,
which this profile does not yet implement.

## Evidence

The cartridge uses actual GB VRAM, SOU_TRN and SOUND packets, with at most three
payload frames and eight commands in a 32 KiB image. Owned track templates are
remapped only at their E0 operands, including byte IDs that equal opcodes or
zero. Payloads cover full, split, reverse-object, interleaved and one-byte-tail
chunk lists. The read-only probe is unchanged from D5.

Nine tests cover 102 physical model/mode/scenario runs:

- Five maps on both models: 2/10, 10/2, 0/255, opcode-valued 224/229, and explicit
  legacy 2/3. Their score/asset hashes differ, while PCM, resolved cache entries,
  voice setup, pitch and tuning are identical for the same logical slot sequence.
- Both models cover inheritance, omitted initial selectors, repeated and maximum
  prefix chains, end/clipped controls and all-slot-2/all-slot-3 playback.
  Native/scalar PCM matches; combined rendering retains bounded frame counts.
- Reordered/repeated song selection, STOP and repeated physical uploads. Selection
  sequences use 125 million clocks so the final song finishes before the
  quiet-tail assertion. Other scenarios use at most 100 million clocks.
- Active generation replacement changes both score IDs and the map, including
  reversed, zero/255 and opcode-valued maps. Initial looping audio has positive
  ENVX when interrupted; replacement admits both slots. STOP/reupload also passes.
- Duplicate/missing maps, unknown first/third-song selectors, nonzero neighboring
  padding, invalid physical SRCNs, and map-only uploads remain muted/unadmitted.
- Cold and active repeated failures with intervening STOP in native/combined
  modes, followed by a changed valid map. Every clear zeros both complete objects,
  blocked intervals contain no PCM, and final hashes include the new map.
- Fragmented semantic retries, including the final one-byte chunk that exercises
  consumed-token synchronization, and frozen D5 positive/negative witnesses.

Every physical scenario requires exact reset and snapshot replay of full state
and PCM. Existing caps remain: 140 million clocks, 4096 restorations, 250000 PCM
frames, 4096 output bytes and a 90-second child timeout. Combined scenarios use
at most 100 million clocks to stay within the PCM cap.

All nine final test methods pass across focused runs, covering all 102 physical
scenarios. Selector/rejection checks pass in 211.04 seconds; repeated recovery
in 420.39 seconds; corrected active replacement in 157.55 seconds; inheritance,
completed selection and export checks in 201.80 seconds. Fragmented retries and
frozen D5 witnesses also pass in their respective focused runs. The complete
physical matrix was checked by method rather than rerunning unchanged methods
after fixture/assertion corrections.
All eleven selected host/transfer, firmware build/reproducibility, fixture,
native-engine and BRR/DSP CTest suites pass (296.85 seconds total). Eleven
build/cap/reproducibility tests across CB through D6 pass (4.34 seconds). Earlier
long physical matrices were not rerun: their images and the probe are unchanged;
D5 still passes live legacy playback and rejects D6 mapping data. The bundled
prototype check and `git diff --check` pass.

No proprietary firmware, bank, sample, descriptor or reference PCM is used.
Independent original-firmware, original-decoder, hardware, acoustic and title
qualification remain outstanding. Production defaults and external overrides
are unchanged.

## Next step

Extend the register-only instrument-10 reference fixture to notes 24 through 36
on both voices and both original models. Instrument 2 already has that chromatic
coverage, while instrument 10 has only three measured notes on voice 2. Use the
new measurements to define the next bounded per-instrument tuning contract
beyond normal/half pitch; keep sample and acoustic equivalence separate.
