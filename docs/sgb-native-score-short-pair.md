# Guarded native consecutive duration-4 events

The native diagnostic now implements the independently authored
[consecutive short-event corpus](sgb-score-short-pair-observations.md).
Channel 2 retriggers its still-keyed voice with duration-4 A0 at tick 64,
releases, then plays duration-4 A1 or rests at tick 68. The following note
applies deferred ED 64 and starts at actual volume 1/1; the rest retains 7/7.
Both tracks terminate at tick 72 and leave silent audio.

This is an opt-in direct-RAM/IPL-trampoline diagnostic. Qualification and
production playback remain false; SGB1/SGB2 program ROMs remain required.

## Source and admission

`scripts/build_sgb_score_short_pair.py` derives the preceding native continuation
source and adds the independently written slot helper in
`firmware/sgb/score_short_pair_slot.asm`. The reproducible image is exactly
4096 bytes, SHA-256
`b27faaa73cb996f6b6fcc0a3bf48160ead49c90ff268c78a14a9e5e615c78e45`.
The preceding 4081-byte image remains
`c0a890f508c30169591ccfcb865113b4198c31af2ef6960fa03256180cbaef70`.

Duration-4 notes are provisionally admitted only in cache slot 4, at cursor 80
with opcode A0 or cursor 82 with opcode A1, tempo 96 and articulation 63/127.
The general duration/pulse table is unchanged. Both provisional gates require
the complete cached profile to pass silent rehearsal before DSP playback.
A short following event additionally requires channel 3 to continue with a
matching duration-4 rest, matching articulation and the measured terminators.
An immediate-final duration-4 boundary, unmatched duration or articulation,
extra continuing event, wrong note opcode or unqualified leading event rejects
silently. Existing geometry, control parsing and subtype guards remain in force.

The small slot helper uses existing padding before the fixed tables; the
preceding label-addressed packing and owned descriptor relocation are retained.
No program, source, pattern, event, tick, physical clock or PCM cap is increased.
The probe derives `short_pair_mode` from the qualified continuation flag C5
and cached following duration 4, and includes it in reset/save/load equality.
It does not create another firmware mailbox or production ownership claim.

## Actual DSP and owned envelopes

Both short notes use a bounded five-pulse native gate. Returning native gates
measure 10012..10019 SPC cycles, while following native gates measure
9950..9957. The opaque originals measure 7426..7433 for the return and
9994..10001 for the following note. The checks retain the existing 4096-cycle
native/reference allowance; they do not claim the original's exact counter,
universal duration-4 behavior or envelope implementation.

The old channel-2 envelope remains positive during inactivity and is retriggered
without an invented KOF. Both new notes attack from zero, reach peak ENVX 127,
and have real timer releases that settle. The returning release precedes the
following KON, and all releases precede final FF. Note cases retain seven
physical release identities; rest cases retain six. Every physical edge is
bound to raw DSP writes, retaining inactive KOF assertions and final FF without
inventing another peer release.

Return and following-note setup retain pitch-before-volume order. Returning
actual volume is 7/7; following actual volume is 1/1 at pitch 1800. Both note
starts issue KOF zero, KOF zero, KON 4 with held mask zero. Rests produce no
new pitch, volume or KON. Final controls remain KOF FF, KOF zero, KON zero,
without setting final-ready mode or retaining a held final note.

Both tails remain silent for 600000 half cycles. Reset, active save/load,
four tail save/load points, source preservation and cached playback guards
are checked. Earlier probe modes retain their schemas and behavior.

## Validation and reproduction

```sh
python3 tests/sgb_score_short_pair_playback_tests.py \
  --probe build-dmg-firmware/gameboy_sgb_score_short_pair_probe
python3 scripts/check_sgb_score_short_pair_playback.py \
  --probe build-dmg-firmware/gameboy_sgb_score_short_pair_probe \
  --trace build-dmg-firmware/gameboy_snes_65c816_apu_trace \
  --firmware-dir roms > /tmp/score-short-pair-playback.json
```

The checker also accepts `--reference` with a retained original-only short-pair
artifact. The native schema is `gbb-spc-score-short-pair-v1`; the combined
schema is `gbb-score-short-pair-playback-reference-v1`. Qualification and
playback remain false. The optional local playback-reference CTest requires
caller-owned original images; public physical tests do not.

All eight new native/original comparisons pass. Maximum raw differences in
SPC cycles are:

| Observation | Maximum difference |
| --- | ---: |
| Note gates | 2911 |
| Onset intervals | 2711 |
| Return control offsets | 105 |
| Return voice setup offsets | 445 |
| Following control offsets | 105 |
| Following voice setup offsets | 576 |
| Final control offsets | 139 |
| Unreleased retrigger interval | 271 |
| Final stop interval | 919 |

All 264 preceding comparisons also pass on this image: pending 32, reverse 24,
peer 16, follow-rest 64, final 16, final-peer 16, final-return 32, final-mixed 16,
direct-return 16, immediate-final short-return 16 and ordinary continuation 16.
No timestamps are shifted, no delays fitted, no timers restarted and no score
pulses discarded. The intermodel allowance remains 2048 cycles.

Eight public physical tests cover reproducibility and bounds, frozen/retrigger
and short-release trajectories, deferred setup/control order, final silence
and reset/save/load, malformed profile rejection, predecessor rejection and
preceding-profile survival, injected faults and forged metadata. The seven
preceding short-pair fixture/observation tests remain applicable.
All 45 public score, fixture and scheduler CTest suites pass on the rebuilt
probes. The bundled prototype hash check and `git diff --check` also pass.

Original children retain the 8-million-instruction, 180-second, 16 MiB CSV and
fewer-than-32768-row caps. Private original instructions, scores, instrument
tables, samples and PCM are not inspected or copied. Original ENVX, PCM
equivalence, hardware behavior and real-title replacement qualification remain
outside this evidence. The bundled prototype is unchanged at SHA-256
`8222797ddeec5681af61fda28c6afc241898887cd0f254ce2f691f30d4b13482`.

## Next integration milestone

The [owned upload/restart/selection diagnostic](sgb-native-score-transport.md)
now exercises this engine through actual SOU_TRN, `$0400` restart and SOUND
selection. It retains the admission guards, rejects malformed banks silently,
and requires fresh ownership readiness. Its integration and lifecycle evidence
is limited to independently authored fixtures. The integration now services
stop, reselection and IPL requests during active playback at timer-poll
boundaries. The separate
[CC directory diagnostic](sgb-native-score-directory.md) now adds bounded
multi-song selection. The separate [two-instrument profile](sgb-native-score-dual-instrument.md)
adds owned uploaded samples and voice-local selection/inheritance. The
[multi-block sample profile](sgb-native-score-brr-chain.md) adds bounded BRR
chains and checked loops. The [D0 profile](sgb-native-score-instrument-profiles.md)
adds bounded uploaded envelopes/direct GAIN and tuning. The D1 profile below adds
non-looping BRR samples. Production selection and general vendor-bank compatibility remain
unqualified.

The documented [title-demand inventory](sgb-vendor-score-demand.md) identifies
additional title obstacles: instrument/sample mapping, echo, full control
curves and broader scheduling; the larger candidate also requires all eight
channels, modulation and subroutine continuation. That inventory is historical,
not a fresh title-validation run for this milestone. Passing an owned transport
and selection test would establish integration before pursuing those remaining
contracts; it would not yet qualify Donkey Kong playback.

The [D1 one-shot profile](sgb-native-score-one-shot.md) now covers bounded
non-looping samples and natural completion. The [D2 profile](sgb-native-score-brr-profiles.md) adds bounded BRR filters/ranges.
The [D3 relocation profile](sgb-native-score-relocated.md) now validates starts/loops
inside fixed sample windows. The [D4 atomic-upload profile](sgb-native-score-atomic-upload.md) now prevents
stale score/sample reuse. The [D5 recovery profile](sgb-native-score-upload-recovery.md) now permits a
fresh complete upload after rejection while keeping SOUND blocked until
admission. Next widen instrument-bank support using measured title demand.
