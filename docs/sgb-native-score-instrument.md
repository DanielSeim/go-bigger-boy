# Bounded uploaded instrument and sample mapping

The opt-in CD diagnostic image extends the [three-song directory](sgb-native-score-directory.md)
with an uploaded instrument-2 descriptor and a single looping BRR block. Both
assets travel through the physical SOU_TRN/IPL path. The descriptor replaces the
native engine's embedded descriptor reads; DSP DIR points at the uploaded source
entry during rendering. The embedded waveform remains in the standalone engine,
but CD playback resolves source 2 through the uploaded directory.

Qualification and production playback remain false, and SGB1/SGB2 program ROMs
remain required. This is a bounded owned-asset mapping milestone. It supports one
instrument ID and one fixed envelope profile; arbitrary descriptors, multiple
instrument IDs, multi-block samples and vendor-bank mapping remain unfinished.
Performance optimization remains deferred.

## Reproducible sources and upload layout

```sh
python3 scripts/build_sgb_score_transport.py --multisong --uploaded-instrument \
  --output /tmp/instrument-host.rom
python3 scripts/build_sgb_score_instrument_fixture.py --timbre step \
  --output /tmp/instrument-game.gb
cmake --build build-dmg-firmware --target gameboy_sgb_score_transport_probe
python3 tests/sgb_score_instrument_tests.py \
  --probe build-dmg-firmware/gameboy_sgb_score_transport_probe
```

Exporters refuse overwrite. The CD image SHA-256 is
`2ad4a5de1b707b27f5b09c08733dd7b5bdae49d411cfbffc1ed71279ed9a718b`.
The CB and CC images remain byte-for-byte unchanged. CD requires the multisong
flag explicitly. Its extra helper occupies `$1800..1BFF`; the combined payload
cap is `$0200..1BFF`. The native engine remains in `$0800..17FF`, with same-width
call/descriptor hooks. Runtime caches remain below `$4C00`.

The GB fixture uploads the 2048-byte score bank to `$2B00`, then a 32-byte object
to `$5000`, then starts the existing `$0400` restart entry. It uses two explicit
IPL blocks and fits the existing 4096-byte SOU_TRN payload.

| Object offset | Meaning | Admitted value |
| --- | --- | --- |
| 0..3 | SRCN, ADSR1, ADSR2, GAIN | `02 8F 6F B8` |
| 4..7 | Reserved directory slots | Zero |
| 8..11 | Source-2 start and loop words | `$5010`, `$5010` |
| 12..15 | Reserved directory slots | Zero |
| 16 | BRR header | `B3`: range 11, filter 0, end and loop |
| 17..24 | Sixteen signed sample nibbles | Opaque uploaded audio data |
| 25..31 | Reserved tail | Zero |

Every structural byte is checked before each native parse/rehearsal, including
all three pre-readiness admissions and every selected-song restart. Rejection
uses the existing muted E2 bridge failure path. Start and loop cannot escape the
nine-byte block; the fixed header cannot chain into unchecked data. Opaque sample
nibbles require no score parsing. DSP descriptor writes retain existing E0
ordering and pre-KON pulse accounting. Existing stop, active selection, IPL
handoff, snapshot and reset contracts remain in effect.

The owned `square` and `step` waves are independently authored filter-0 nibble
sequences. No proprietary ROM, descriptor, sample or reference PCM is included.
A missing object is tested as an all-zero upload. This does not establish atomic
replacement semantics for arbitrary partial uploads or prove that stale objects
are invalidated when a later upload omits their block.

## Validation

The five public tests exercise 44 physical model/mode/scenario runs, each with
uninterrupted, restored and cold-reset execution. Both SGB models must admit
silently, render the two uploaded waves to distinct PCM digests, and retain exact
native/scalar PCM parity. Combined resampling has its own exact lifecycle
comparison and bounded frame counts. Active three-song switching, stop/resume
and repeated upload must preserve expected interruption flags and positive ENVX.
Nine malformed-object cases per model cover source ID, ADSR, gain, start, loop,
BRR header, reserved bytes, tail padding and a zero/missing object; these must
reject before any song is admitted and produce no audio.

All ten selected instrument, directory, transport, host, transfer, firmware,
fixture and native-engine CTest suites pass. CLI reproducibility and overwrite
protection, the bundled prototype hash check and `git diff --check` also pass.

These checks establish owned-asset protocol and emulator behavior. They do not
independently qualify original sample decoding, original instrument maps,
acoustic equivalence, hardware behavior or commercial titles.

## Next step

The [multi-block sample profile](sgb-native-score-brr-chain.md) now adds one to
four BRR blocks per source and validated loop points. The [D0 profile](sgb-native-score-instrument-profiles.md)
adds bounded uploaded envelopes/direct GAIN and tuning. The D1 profile below adds non-looping BRR samples while retaining these guards.

The [D1 one-shot profile](sgb-native-score-one-shot.md) now covers bounded
non-looping samples and natural completion. The [D2 profile](sgb-native-score-brr-profiles.md) adds bounded BRR filters/ranges.
Next add bounded relocatable sample-directory entries.
