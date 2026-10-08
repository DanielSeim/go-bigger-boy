# Two uploaded instruments and voice-local inheritance

The opt-in CE diagnostic image extends the [uploaded instrument profile](sgb-native-score-instrument.md)
with instrument IDs 2 and 3, two bounded BRR samples, and ordered E0 selections at
native event boundaries. Events without E0 retain each voice's DSP descriptor.
The two voices inherit independently across notes, rests and sparse patterns.
Executed end-event selections carry into the next pattern; selections beyond a
clipped note remain unexecuted.

Qualification and production playback remain false. SGB1/SGB2 program ROMs
remain required. This owned two-instrument profile does not establish vendor
instrument maps, original tuning/envelopes, acoustic equivalence, hardware
behavior or commercial-title compatibility. Performance work remains deferred.

## Reproducible build and bounded assets

```sh
python3 scripts/build_sgb_score_transport.py --multisong --uploaded-instrument \
  --two-instruments --output /tmp/dual-host.rom
python3 scripts/build_sgb_score_dual_instrument_fixture.py \
  --profile repeat --output /tmp/dual-game.gb
cmake --build build-dmg-firmware --target gameboy_sgb_score_transport_probe
python3 tests/sgb_score_dual_instrument_tests.py \
  --probe build-dmg-firmware/gameboy_sgb_score_transport_probe
```

Exporters refuse overwrite. The CE image SHA-256 is
`ba442d2e0e6ae56d2db1d5295832683a907b79b05cdbab41efe1b640fe1d4802`.
CB, CC, CD and the standalone native engine remain unchanged. CE requires the
multisong and uploaded-instrument flags. The native engine retains its fixed
`$0800..17FF` allocation. The helper remains within `$1800..1BFF`, and the
combined payload retains the CD `$0200..1BFF` cap. Assembly rejects overlaps.

The owned fixture uploads the same 2048-byte three-song bank to `$2B00`, then a
64-byte object to `$5000`, then starts `$0400`. It fits the existing 4096-byte
SOU_TRN payload and uses the physical IPL loader without injected RAM.

| Object offset | Meaning | Admitted value |
| --- | --- | --- |
| 0..3 | Instrument 2 descriptor | `02 8F 6F B8` |
| 4..7 | Instrument 3 descriptor | `03 8F 6F B8` |
| 8..11 | Source-2 start and loop | `$5020`, `$5020` |
| 12..15 | Source-3 start and loop | `$5030`, `$5030` |
| 16..31 | Reserved bytes | Zero |
| 32..40 | Source-2 looping BRR block | Header `B3`, eight opaque data bytes |
| 41..47 | Reserved bytes | Zero |
| 48..56 | Source-3 looping BRR block | Header `B3`, eight opaque data bytes |
| 57..63 | Reserved bytes | Zero |

Both descriptors and all sample structure are checked before every native
parse/rehearsal, including all three directory admissions and each selection.
Both samples use range 11, filter 0, end and loop bits. Fixed start/loop words
bound each source to its own nine-byte block. Instrument IDs outside 2/3 reject
before readiness, including references confined to an unselected third song.
The source objects use independently authored square and step waves; swapping
those waves changes PCM without changing the selected source IDs.

## Ordered event caches and inheritance

The CE parser permits E0 at the same native control-dispatch boundary as pan
and volume. The existing per-track limit of 32 controls, eight timed events,
finite nonnested calls, source bounds and read budget remain enforced.

The native `$4A00` cache still records each event's E0 count. An additional
bounded `$6000..7FFF` cache records the ordered IDs: its 32 pages correspond to
prefix ordinals, and each page is indexed by the native expanded event slot.
Before storing, the helper requires an ordinal below 32 and an ID of 2 or 3.
This region is separate from code, score source, sample objects and the existing
runtime caches. Echo writes remain disabled under the existing DSP setup.

Runtime replay reads exactly the cached count and writes each descriptor in
prefix order, retaining the preceding pre-KON timer-pulse accounting. Count
zero performs no descriptor write. DSP source state carries independently for
voices 2 and 3. Setup resets both voices to instrument 2 on each fresh score;
active selection and re-upload do not inherit a preceding song's instrument.
End-event counts are cached separately and replayed only for the ending voice
chosen by the existing first-end/channel-2-priority rule.

Stale cache bytes beyond an event's count are never consumed. This does not
establish atomic replacement or invalidation of objects omitted by arbitrary
later partial uploads. Envelopes are still fixed to the owned profile, tuning
uses the preceding finite pitch table, and each sample is one block.

## Public validation

Seven public tests exercise 78 physical model/mode/scenario runs, each comparing
uninterrupted execution with restored and cold-reset PCM and final state:

- Silent admission on SGB1 and SGB2.
- Eight selection profiles on both models with native/scalar parity; the
  switching profile also runs combined resampling.
- Per-event selection, voice-local inheritance, repeated E0 prefix order,
  maximum admitted control counts, end-event inheritance and clipped tails.
- Swapped uploaded waveform mapping on both models.
- Active three-song switching in all three modes, plus stop/resume and active
  repeated upload on both models. Mid-note SOUND waits six LCD frames;
  SOU_TRN is requested after four because its host handoff adds latency.
- Gate-boundary selection and upload at the preceding eight/six-frame timings,
  retaining the measured model-specific ENVX behavior at ticks 14..16. SGB1
  selection and both models' upload see zero voice-2 ENVX; SGB2 selection sees
  the following attack. Both paths must finish and replay exactly.
- Twelve malformed-object/reference/control cases per model, including invalid
  third-song references and an overlong E0 prefix sequence. Rejection is silent.

The independent symbolic scheduler checks final ticks 72/72/76 for all eight
owned profiles. The physical probe additionally observes SRCN while ENVX is
positive, checks the expected voice/source mask and final inherited sources,
reads prefix IDs/counts without mutation, and restores at newly observed
instrument phases. Repeated prefixes must retain their actual ordered IDs;
executed-end and clipped-tail cases must leave different inherited sources.
Native/scalar PCM must match exactly; combined output has exact lifecycle
comparison and bounded resampling counts. Complete scores require a silent tail
of at least one million physical master clocks.

All eleven selected instrument, directory, transport, host, transfer, firmware,
fixture and native-engine CTest suites pass. The preceding CB/CC/CD suites cover
150 additional model/mode/scenario runs with the expanded physical probe. CLI
byte reproducibility and overwrite protection, the bundled prototype hash check
and `git diff --check` also pass.

No proprietary program, bank, instrument, sample or reference PCM is a test
input. These are public owned-asset and emulator checks; independent original or
hardware qualification remains outstanding.

## Next step

The [multi-block sample profile](sgb-native-score-brr-chain.md) now adds one to
four BRR blocks per source and validated loop points. Next add bounded uploaded
per-instrument envelopes and tuning while keeping the existing lifecycle and
sample guards.
