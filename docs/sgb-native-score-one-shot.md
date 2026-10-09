# Bounded non-looping BRR samples

The opt-in D1 diagnostic image extends the [uploaded envelope/tuning profile](sgb-native-score-instrument-profiles.md)
with independently selected looping and one-shot sources. A one-shot's terminal
end-without-loop header lets the DSP silence it while the native note gate and
score continue. No firmware polling or forced key-off is used to end the sample.

Qualification and production playback remain false. SGB1/SGB2 program ROMs remain
required. This covers owned filter-0, range-11 data, two source IDs, fixed windows
and one to four blocks per source. General sample banks, other filters/ranges,
vendor instrument maps, echo and broader scheduling remain unfinished.
Performance optimization remains deferred.

## Reproducible image and asset contract

```sh
python3 scripts/build_sgb_score_transport.py --multisong --uploaded-instrument \
  --two-instruments --multiblock --instrument-profiles --one-shot --output /tmp/one-shot-host.rom
python3 scripts/build_sgb_score_one_shot_fixture.py --sample-profile intro \
  --profile baseline --score-profile switch --output /tmp/one-shot-game.gb
cmake --build build-dmg-firmware --target gameboy_sgb_score_transport_probe
python3 tests/sgb_score_one_shot_tests.py \
  --probe build-dmg-firmware/gameboy_sgb_score_transport_probe
```

The D1 image SHA-256 is
`b1f9b27f541165e18b48a660dd9473a859633374bea11645e7e664f3036639fb`.
Exporters refuse overwrite. All five preceding flags are required; preceding
CB/CC/CD/CE/CF/D0 images and the bundled prototype retain their hashes. The
combined code cap remains `$0200..1BFF`, the native region `$0800..17FF`, the
score bank 2048 bytes and the uploaded asset object 192 bytes. Both physical
upload blocks and the restart header fit the existing 4096-byte SOU_TRN payload.
No production emulator path changes and no RAM is injected by the probe.

| Object offsets | Meaning | Admitted values |
| --- | --- | --- |
| 0..7 | Source IDs and envelope descriptors | Preceding D0 contract |
| 8..15 | DSP start/loop words | Starts `$5040`/`$5080`; bounded loop words |
| 16, 17 | Block counts | Each 1..4 |
| 18, 22 | Tuning flags | 0 normal, 1 half pitch |
| 19, 23 | Source modes | 0 looping, 1 one-shot |
| 20, 21, 24..63 | Reserved | Zero |
| 64..127, 128..191 | Two BRR windows | Declared blocks, then zero padding |

Intermediate headers must be `B0`. A looping source must end with `B3` and its
loop pointer must name a declared header in its own window. A one-shot source
must end with `B1` and its loop word must equal its start word. This canonical
word keeps any redirected, inaudible DSP reads within the validated source;
it does not enable audible looping. A one-shot does not accept an interior
loop word, even when that word is aligned and inside its sample.

Modes greater than 1, header/mode disagreement, early end headers, missing
terminal ends, invalid start/loop words, invalid counts, unsupported filters or
ranges, nonzero padding/reserved bytes, and malformed envelope/tuning data
reject the entire object. Both sources are checked before every rehearsal,
including all three silent directory admissions and every selection. Validation
uses EA for the current source mode, separate from voice-local tuning EC/ED.

The DSP handles natural completion. Native duration, gates, articulation,
ordered E0 replay, voice-local instrument inheritance, executed end controls,
clipped tails, cooperative stop/selection and IPL reupload remain in use.
A completed sample can be inaudible while the score is still active; transport
commands must continue to work during that interval. Looping sources retain
the preceding positive-envelope active-command evidence.

## Owned fixtures and physical evidence

Eight sample profiles cover both modes and all admitted block counts:

| Profile | Counts, sources 2/3 | Modes, sources 2/3 |
| --- | --- | --- |
| loop | 3, 4 | loop, loop |
| one | 1, 1 | one-shot, one-shot |
| pair | 2, 2 | one-shot, one-shot |
| triple | 3, 3 | one-shot, one-shot |
| intro | 3, 4 | one-shot, one-shot |
| full | 4, 4 | one-shot, one-shot |
| mixed2 | 3, 4 | one-shot, loop |
| mixed3 | 3, 4 | loop, one-shot |

The waveform bytes remain independently authored. A single terminal block in
the owned `one` fixture produces no audible PCM: its non-looping header silences
the envelope before audible data. Longer one-shots produce nonzero PCM and then
silence. This is a tested emulator contract, not hardware/acoustic qualification.

Six tests cover 120 physical model/mode/scenario runs, each with uninterrupted,
restored and cold-reset execution:

- Silent admission of both sources on SGB1/SGB2.
- Every sample profile in native mode on both models; loop, one, full and mixed3
  also in scalar and combined modes. Native/scalar PCM must match exactly;
  combined mode retains bounded resampling counts.
- Stable first/second-note observations at symbolic ticks 10 and 26: each
  one-shot must have zero ENVX, latched ENDX, a positive native gate counter and
  a clear KOF bit simultaneously. Looping sources must retain positive ENVX at
  the same points. ENDX alone is insufficient because looping ends also set it.
  The independent score schedules still finish at ticks 72/72/76. Completed
  scores leave zero ENVX and at least one million master clocks of silent tail.
- Uploaded ADSR/tuning and direct GAIN, ordered repeated E0, sparse inheritance,
  executed end selection, clipped tails, both instruments on both voices and
  initial notes without E0. Observed source IDs must match the expected replay.
- Active selection, stop/resume and repeated physical upload after one-shot
  completion while the score is active; the same operations with a live looping
  source retain positive ENVX. Mixed-source selection has scalar/combined checks.
- Twenty-six malformed cases per model, covering source modes, terminal/header
  disagreement, early/missing ends, noncanonical/cross-source/high-byte loop
  words, counts, filter/range, padding, ADSR/tuning, reserved data and a missing
  object. Rejection must be silent and precede every directory admission.

The read-only probe includes source modes, settled envelope/source/gate/ENDX/KOF
observations and natural-end masks in exact lifecycle comparison with full
serialized state and PCM. Snapshot phases include ENDX and ENVX-zero transitions and settled natural-end
observations.
All seventeen selected one-shot, sample, instrument, directory, transport, host,
transfer, firmware, fixture, native-engine and DSP BRR/end-state/renderer CTest
suites pass. The preceding profiles retain 430 physical model/mode/scenario
checks with the expanded probe. CLI byte reproducibility and overwrite
protection, prior image hashes, the bundled prototype check and `git diff --check`
also pass.

No proprietary firmware, bank, descriptor, sample or reference PCM is used.
Original-decoder, hardware, acoustic and title qualification remain outstanding.
Partial uploads still lack atomic asset invalidation.

## Next step

The [D2 BRR profile](sgb-native-score-brr-profiles.md) adds bounded filters/ranges,
independent decoded-sample arithmetic checks and physical playback evidence.
The [D3 relocation profile](sgb-native-score-relocated.md) now validates starts/loops
inside fixed sample windows. The [D4 atomic-upload profile](sgb-native-score-atomic-upload.md) now prevents
stale score/sample reuse. The [D5 recovery profile](sgb-native-score-upload-recovery.md) now permits a
fresh complete upload after rejection while keeping SOUND blocked until
admission. Next widen instrument-bank support using measured title demand.
