# Opaque short-return observations with ordinary continuation

Both original models apply a deferred ED 64 volume control when a duration-4
return is followed by an ordinary note. That note starts at actual volume 1/1
and releases before final termination. A continuing rest writes no new pitch
or volume and retains channel-2 volume 7/7. This differs from the
[immediate-final-ready profile](sgb-native-score-short-return.md), whose pending
final note retains volume 7/7.

## Independently authored corpus

`build_sgb_score_short_continue_fixture.py` derives the preceding owned short
return banks. Only channel 3's third-pattern termination changes: after ED 64
at tick 68 it continues with a duration-8/16 rest, matching channel 2's next
note or rest. The 2048-byte banks retain the two preceding patterns, initial
articulation 127, tempo 96, instrument 2, pan 10, track volume 127 and song
volume 160. Channel 2's duration-24 note at tick 16 remains clipped at tick 32,
then stays keyed while channel 3 alone plays through tick 64.

At tick 64, channel 2 returns with duration-4 A0, pitch 1700, and channel 3
executes a duration-4 C9 rest. Returning articulation is 63 or 127. At tick 68,
both channels execute ED 64. Channel 2 then executes duration-8/16 A1, pitch
1800, or C9; channel 3 executes C9 with the same duration and articulation.
The score ends at tick 76 or 84.

The independent symbolic scheduler confirms starts 0/32/64, masks 12/8/12,
ten expanded note/rest events and ordinary progression at tick 68. There is
no pending final boundary event. Two articulations, two continuation durations,
two event kinds and both original models give 16 fresh bounded executions.
Every observation is bound to its independently authored cartridge hash.

## Measured setup and release

The return preserves the preceding measured register lifecycle. Pitch writes
precede volume writes; return setup offsets from KON are -1479, -1443, -651
and -509 SPC cycles for PITCHL2, PITCHH2, VOLL2 and VOLR2. Return values are
164, 6, 7 and 7. Controls are KOF zero at offsets -156 and -39, then KON 4.
There is no intervening KOF of the old channel-2 note. The one unreleased
retrigger joins onset index 1 to index 4 and spans 262274..262276 cycles.

The returning duration-4 gate remains 7426..7427 cycles at articulation 63
and 7432..7433 at articulation 127. Its release precedes ordinary progression.
The following note starts 19877..19887 cycles after returning KON and has:

| Offset from following KON in SPC cycles | DSP address | Value |
| --- | --- | --- |
| -1609 | PITCHL2, 34 | 8 |
| -1573 | PITCHH2, 35 | 7 |
| -651 | VOLL2, 32 | 1 |
| -509 | VOLR2, 33 | 1 |

Following controls again are KOF zero, KOF zero, KON 4 at offsets -156, -39
and zero. The following note has a real timer release:

| Articulation | Following duration | Gate in SPC cycles |
| --- | --- | --- |
| 63 | 8 | 20230..20232 |
| 63 | 16 | 46860..46864 |
| 127 | 8 | 30480..30483 |
| 127 | 16 | 75955..75996 |

These following gates fit the existing duration-8/16 note contract without
adding a duration-4 entry to the general pulse table. Neither a following rest
nor channel 3's continuing rest writes new pitch or volume. Actual final
volumes are 1/1 on both channels for following-note cases and 7/7, 1/1 for
following-rest cases. Raw zero control pulses are retained during rest and
post-stop observation; absence of a following KON does not imply absence of
those writes.

All following-note cases have seven physical timer releases; rest cases have
six. Inactive channel-3 KOF assertions do not invent physical releases.
No voice remains pending at final stop or in the bounded tail. Final controls
are KOF FF, KOF zero at +117 cycles and KON zero at +156 cycles. Stop occurs
62764..62771 cycles after return for continuation duration 8 and
105770..105777 for duration 16. Post-stop observations span
2672722..4467286 cycles with no nonzero control writes after the final pulse.

Maximum intermodel differences are 49 cycles for gates, 48 for onset intervals,
seven for stop, two for the unreleased retrigger and zero for return, following
and final setup/control offsets. These remain within the unchanged 2048-cycle
intermodel allowance. No timestamp shifting, phase fit or delay calibration is
used.

## Validation and scope

```sh
python3 tests/sgb_score_short_continue_tests.py
python3 scripts/check_sgb_score_short_continue_reference.py \
  --trace build-dmg-firmware/gameboy_snes_65c816_apu_trace \
  --firmware-dir roms > /tmp/score-short-continue-observations.json
```

Seven public tests cover reproducibility/input bounds, independent scheduler
geometry, the synthetic two-model ledger, forged setup/control/gate/lifecycle
observations, completeness/model/hash binding, intermodel drift and trace
header/byte/row/order/value bounds. The native short-return suite additionally
checks silent rejection of all eight continuing banks: status 226, no accepted
key writes and no nonzero owned PCM. The preceding native image still passes
all 16 retained immediate-final short-return comparisons.
All seven focused fixture, scheduler and native direct/mixed/short-return
CTest suites pass. The bundled prototype hash check and `git diff --check`
also pass.

The optional private-reference CTest uses schema
`gbb-score-short-continue-observation-v1`, with qualification and playback
false. Original children retain the 8-million-instruction, 180-second,
16 MiB CSV and fewer-than-32768-row caps. Source and native bounds are unchanged.
The native 4089-byte image remains SHA-256
`d86d1587cb4329b9ded58e42a14d589b66af92fca370badcf3e78c2966139ba0`;
the bundled prototype is unchanged. SGB1/SGB2 program ROMs remain required.

These observations establish register-write timing and lifecycle, not original
ENVX, PCM equivalence, hardware behavior or real-title qualification. Private
original instructions, scores, instrument tables, samples and PCM are not
inspected or copied.

Next, implement this guarded continuing profile: preserve the unreleased
return, apply the deferred control when the following note starts, retain the
following rest's actual volume, and release ordinary notes before final stop.
Validate owned attack/release, audio, reset/save/load and the preceding
reference corpus before extending selection.
