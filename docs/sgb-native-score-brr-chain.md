# Bounded multi-block BRR samples and loop points

The opt-in CF diagnostic image extends the [two-instrument profile](sgb-native-score-dual-instrument.md)
with one to four BRR blocks per source and loop pointers to any declared block.
Both uploaded sources are checked before any song is admitted. The existing
ordered E0 replay, voice-local inheritance, first-end rules and cooperative
stop/selection/IPL paths remain in use.

Qualification and production playback remain false. SGB1/SGB2 program ROMs
remain required. This profile covers owned filter-0, range-11 looping samples,
two instrument IDs, fixed start addresses and the preceding finite score
subset. General sample banks, other filters/ranges, one-shot samples, uploaded
envelopes/tuning, echo and broader scheduling remain unfinished. Performance
optimization remains deferred.

## Reproducible build and upload layout

```sh
python3 scripts/build_sgb_score_transport.py --multisong --uploaded-instrument \
  --two-instruments --multiblock --output /tmp/brr-host.rom
python3 scripts/build_sgb_score_brr_chain_fixture.py --sample-profile intro \
  --score-profile switch --output /tmp/brr-game.gb
cmake --build build-dmg-firmware --target gameboy_sgb_score_transport_probe
python3 tests/sgb_score_brr_chain_tests.py \
  --probe build-dmg-firmware/gameboy_sgb_score_transport_probe
```

Exporters refuse overwrite. The CF image SHA-256 is
`bfc7f800a706012540a735bf8ec4284e8379ddbe555d2590101dd3e0c20fe4fb`.
CB, CC, CD, CE and the standalone native engine remain unchanged. CF requires
all three preceding profile flags. The combined code cap remains
`$0200..1BFF`, with the native engine in `$0800..17FF` and sample helpers above
`$1800`. The ordered E0 cache remains `$6000..7FFF`, separate from the samples.

The GB fixture uploads the same 2048-byte bank to `$2B00`, a 192-byte object to
`$5000`, then starts `$0400`. Both physical IPL blocks and the restart header fit
the existing 4096-byte SOU_TRN payload. No RAM is injected by the probe.

| Object offset | Meaning | Admitted value |
| --- | --- | --- |
| 0..3 | Instrument 2 descriptor | `02 8F 6F B8` |
| 4..7 | Instrument 3 descriptor | `03 8F 6F B8` |
| 8..11 | Source-2 start and loop | Start `$5040`; loop at a declared block |
| 12..15 | Source-3 start and loop | Start `$5080`; loop at a declared block |
| 16, 17 | Source-2/source-3 block counts | Each 1..4 |
| 18..63 | Reserved bytes | Zero |
| 64..127 | Source-2 window | Declared blocks, then zero padding |
| 128..191 | Source-3 window | Declared blocks, then zero padding |

Each block is nine bytes: one header followed by eight opaque data bytes.
Intermediate headers must be `B0` (range 11, filter 0, no end/loop flags); the
final header must be `B3` (end and loop). A declared source therefore consumes
9..36 bytes within its own 64-byte window. Remaining bytes must be zero.

Validation requires fixed start words and loop high byte `$50`. For each source,
the helper walks exactly its bounded block count and requires the loop low byte
to match one visited header. This admits aligned interior loops and a final-block
loop while rejecting padding, unaligned positions, another source and out-of-page
pointers. An early terminator or a missing final end/loop flag rejects the entire
object. Count 0 or greater than 4 rejects before any block traversal.

Both sources pass this check before every native parse/rehearsal: all three
pre-readiness directory admissions and each subsequent song selection. A bad
second source prevents audio even when the requested score would begin with
instrument 2. E2 rejection retains muted DSP state and clears fresh readiness.

Validation uses E0/E1/E2/E7/E8/E9 scratch, separate from E3..E6 runtime descriptor
and prefix-cache scratch. Source data and loop words remain uploaded assets;
DSP DIR `$50` resolves them directly. No sample is copied into the engine's
embedded owned waveform. Omitted objects in arbitrary later partial uploads
still have no atomic invalidation guarantee.

## Owned samples and public evidence

Six independently authored sample profiles cover the bounded layouts:

| Profile | Block counts, sources 2/3 | Loop block indices, sources 2/3 |
| --- | --- | --- |
| single | 1, 1 | 0, 0 |
| pair | 2, 2 | 0, 0 |
| intro | 3, 4 | 1, 2 |
| full | 4, 4 | 0, 0 |
| tail | 4, 4 | 3, 3 |
| mixed | 1, 4 | 0, 2 |

The blocks use owned square, inverted, reduced-amplitude and stepped nibble
sequences. `full` and `tail` contain identical block data and differ only in
loop words; their physical DSP PCM must differ. All six profiles must produce
distinct PCM digests within each model.

Six public tests cover 104 physical model/mode/scenario runs, each with
uninterrupted, restored and cold-reset execution:

- Silent pre-SOUND admission of both sources on both models.
- All six sample profiles in native, scalar and combined modes on SGB1/SGB2.
- Instrument inheritance, ordered repeated E0, executed-end selections and
  clipped tails while the longer samples play.
- Active three-song switching in all three modes, plus stop/resume and repeated
  physical upload on both models. The preceding six-frame SOUND/four-frame
  upload timings require positive voice-2 ENVX before the gate end.
- Twenty-four malformed cases on each model: invalid counts, start pointers,
  unaligned/padding/cross-source/out-of-page loops, early and missing ends,
  missing loop flags, unsupported filter/range, nonzero padding/reserved data
  and an all-zero missing object. These must reject before any song admission
  and produce no audio.

The physical probe reads the uploaded block counts and loop words through its
read-only RAM observations and includes them in exact lifecycle comparison.
Instrument/source masks and prefix caches retain the preceding checks. Native
and scalar PCM must match exactly; combined output retains exact lifecycle
comparison and bounded resampling counts. Completed scores must reach their
independent symbolic tick counts and leave at least one million master clocks
of silent tail. Execution, snapshot, PCM and child timeout caps remain in force.

All twelve selected sample, instrument, directory, transport, host, transfer,
firmware, fixture and native-engine CTest suites pass. The preceding CB/CC/CD/CE
suites cover 228 additional model/mode/scenario runs with the expanded probe.
CLI byte reproducibility and overwrite protection, the bundled prototype hash
check and `git diff --check` also pass.

No proprietary firmware, bank, sample, descriptor or reference PCM is a test
input. These checks establish the owned upload/loop and emulator lifecycle
contract; independent original-decoder, hardware, acoustic and title
qualification remain outstanding.

## Next step

Bounded uploaded envelope/direct-GAIN and tuning profiles are covered by the
[next D0 diagnostic](sgb-native-score-instrument-profiles.md). The D1 profile below
adds bounded non-looping BRR samples with physical natural-completion, gate,
instrument-selection and lifecycle evidence. General vendor instrument maps
remain a separate qualification boundary.

The [D1 one-shot profile](sgb-native-score-one-shot.md) now covers bounded
non-looping samples and natural completion. The [D2 profile](sgb-native-score-brr-profiles.md) adds bounded BRR filters/ranges.
The [D3 relocation profile](sgb-native-score-relocated.md) now validates starts/loops
inside fixed sample windows. Next make replacement upload admission atomic.
