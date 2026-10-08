# Opaque consecutive duration-4 observations

Both original models release a second duration-4 note after a short return,
with a different raw gate interval from the returning note. The following
note applies the deferred ED 64 control and starts at actual volume 1/1.
These observations extend the
[native duration-8/16 continuation profile](sgb-native-score-short-continue.md);
the subsequent [native short-pair diagnostic](sgb-native-score-short-pair.md)
now implements and validates this bounded profile separately.

## Independently authored corpus

`build_sgb_score_short_pair_fixture.py` derives the preceding owned
continuation banks at following duration 8 and changes only the two following
event durations to 4. Banks remain 2048 bytes. Initial articulation 127,
tempo 96, instrument 2, pan 10, track volume 127, song volume 160 and the
preceding two patterns remain unchanged. Channel 2's clipped duration-24
note remains keyed while channel 3 plays alone through tick 64.

At tick 64, channel 2 returns with duration-4 A0, pitch 1700, and channel 3
executes duration-4 C9. At tick 68 both channels execute ED 64, then channel 2
executes duration-4 A1, pitch 1800, or C9. Channel 3 continues with duration-4
C9. Both tracks use returning articulation 63 or 127 and end at tick 72.

The independent symbolic scheduler confirms starts 0/32/64, masks 12/8/12,
ten expanded note/rest events, ordinary progression at tick 68 and final tick
72. No pending final boundary event is introduced. Two articulations, note/rest
cases and both original models give eight fresh bounded executions, each bound
to its independently authored cartridge hash.

## Measured gates and actual DSP setup

| Articulation | Returning duration-4 gate | Following duration-4 gate |
| --- | --- | --- |
| 63 | 7426..7427 SPC cycles | 9994..9995 SPC cycles |
| 127 | 7432..7433 SPC cycles | 10000..10001 SPC cycles |

The following gate is 2568 SPC cycles longer than the returning gate in every
note case. This is a raw position-specific observation, not evidence for a
universal duration-4 counter or interpolation law. The reference contract
uses separate 6144..8192-cycle returning and 8192..10240-cycle following gate
windows only through an explicit second short-event selector. Preceding
duration-8/16 defaults and the general native
pulse table remain unchanged.

The old channel-2 voice still has no invented release before returning KON.
Its one unreleased retrigger joins onset indices 1 and 4, spanning
262274..262276 SPC cycles and equaling the sum of the three intervening onset
intervals. The returning short note then has a real timer release before the
following note, which starts 19877..19887 cycles after returning KON.

Return setup remains pitch before volume, at offsets -1479/-1443/-651/-509
cycles from KON, with PITCHL2/PITCHH2/VOLL2/VOLR2 values 164/6/7/7.
Following note setup is:

| Offset from following KON in SPC cycles | DSP address | Value |
| --- | --- | --- |
| -1609 | PITCHL2, 34 | 8 |
| -1573 | PITCHH2, 35 | 7 |
| -651 | VOLL2, 32 | 1 |
| -509 | VOLR2, 33 | 1 |

Both note starts issue KOF zero, KOF zero, KON 4 at offsets -156, -39 and zero.
Following rests issue no new pitch, volume or KON; raw zero control pulses
remain in the trace. Final actual volumes are 1/1 on both channels for note
cases and 7/7, 1/1 for rest cases.

Note cases retain seven physical timer-release identities; rest cases retain
six. All releases occur before final stop. Inactive peer KOF assertions do
not invent another physical release. No voice is pending at final stop or
unreleased in the bounded tail. Final controls remain KOF FF, KOF zero at
+117 cycles and KON zero at +156 cycles. Stop occurs 40236..40245 cycles after
returning KON. Post-stop observations span 2738260..4489815 cycles, with no
subsequent nonzero control writes.

Maximum intermodel differences are 49 cycles for gates, 48 for onset intervals,
four for stop, two for the unreleased retrigger and zero for all setup/control
offsets. The second short gate itself differs by one cycle. All are within
the unchanged 2048-cycle intermodel allowance. No timestamp shift, fitted
delay or phase calibration is used.

## Validation and scope

```sh
python3 tests/sgb_score_short_pair_tests.py
python3 scripts/check_sgb_score_short_pair_reference.py \
  --trace build-dmg-firmware/gameboy_snes_65c816_apu_trace \
  --firmware-dir roms > /tmp/score-short-pair-observations.json
```

Seven public tests cover owned fixture/input bounds, symbolic geometry,
synthetic release/retrigger ledgers, closed preceding defaults, forged gate,
setup, duration, release and tail observations, model/hash completeness,
intermodel drift and trace header/byte/row/order/value bounds. The native
preceding continuation suite additionally checks silent rejection of all four new banks:
status 226, no accepted key writes and no nonzero owned PCM. The preceding
native image still passes its 16 retained continuation reference comparisons.
All seven focused fixture, scheduler and native direct/short/continuation CTest
suites pass. The bundled prototype hash check and `git diff --check` also pass.

The optional original-only local-reference CTest uses schema
`gbb-score-short-pair-observation-v1`, with qualification and playback false.
Original children retain the 8-million-instruction, 180-second, 16 MiB CSV and
fewer-than-32768-row caps. Source and native bounds are unchanged. The native
4081-byte image remains SHA-256
`c0a890f508c30169591ccfcb865113b4198c31af2ef6960fa03256180cbaef70`;
the bundled prototype is unchanged. SGB1/SGB2 program ROMs remain required.

These checks establish register-write lifecycle and timing, not original ENVX,
PCM equivalence, hardware behavior or real-title qualification. Private original
instructions, scores, instrument tables, samples and PCM are not inspected or
copied.

The subsequent native diagnostic validates owned attacks, timer releases,
deferred volume, final silence and reset/save/load, and passes all 264 preceding
reference comparisons. Its separate implementation evidence and next
integration milestone are documented in the linked native profile.
