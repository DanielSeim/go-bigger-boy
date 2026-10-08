# Experimental mixed-articulation return before final note/rest

The isolated renderer now reproduces the measured release of a held
articulation-127 voice when it returns from inactivity through an
articulation-63 rest. The rest changes the existing timer gate without KON.
The subsequent pending articulation-63 note/rest retains the previously
measured final KOF FF/00 and KON behavior. This extends the
[uniform-articulation returning-rest diagnostic](sgb-native-score-final-return.md),
whose source and image still build independently.

## Owned corpus and opaque observations

`build_sgb_score_final_mixed_fixture.py` derives the preceding owned 2048-byte
three-pattern banks. The first two patterns retain articulation 127. Both
returning rests and channel 2's pending final event use articulation 63.
Tempo remains 96, pattern starts are 0/32/64, and channel masks are 12/8/12.
Channel 2's second duration-24 note is clipped at tick 32 and stays keyed while
channel 3 plays two solo duration-16 notes. At tick 64 both channels return
with matching duration-4/8 rests. At tick 68/72, channel 2 executes ED 64 and
its pending duration-8/16 A0 note or C9 rest, while channel 3 executes ED 64
and terminates the score. No instrument or sample data comes from an original.

Two rest durations, two pending durations, two pending event kinds and both
original models give 16 fresh opaque executions. The old channel-2 voice
releases before final termination in every case. Relative to channel 3's
preceding onset at tick 48, the observed old-voice KOF intervals are:

| Returning articulation-63 rest | Original interval in SPC cycles | Native timer pulses |
| --- | --- | --- |
| Duration 4 | 96827..96835 | 5 |
| Duration 8 | 107071..107072 | 9 |

The duration-4 release is nearly unchanged from articulation 127. Duration 8
shortens by approximately six timer pulses. Nine pulses is qualified here for
an articulation-63 rest acting on this previously held voice; it is separate
from the ten-pulse duration-8 articulation-63 note profile. Duration-4 notes,
other tempi, reverse articulation changes and other return geometries remain
unqualified. The original interval includes the 16 ticks before the rest;
its checker binds it to raw note gates and actual onset intervals rather than
inventing an original rest timestamp.

All 96 preceding releases are timer KOF writes. Neither rest writes DSP pitch,
volume or KON. There are no unresolved retriggers or voices pending at the
final FF write. Final controls remain KOF FF at offset 0, KOF zero at 117 SPC
cycles and KON 4 for the pending note or zero for the pending rest at 156 cycles.
The pending note writes pitch 1700 and retains actual channel-2 volume 7/7,
although its deferred ED control reports track volume 64. Channel 3 retains
actual volume 1/1. All eight final notes remain unreleased during the bounded
post-stop window; all eight final rests finish without an unresolved voice.
Those windows span 2737204..4511800 SPC cycles. Original PCM, original
instrument/envelope equivalence and indefinite sustain are not established.

Maximum intermodel differences are 49 cycles for gates, 48 for onset intervals,
7 for final stop intervals and zero for pitch/control offsets. These fit the
existing 2048-cycle intermodel allowance.

## Independent implementation and validation

`build_sgb_score_final_mixed.py` adds checked hooks and independently written
`score_final_mixed_init.asm` and `score_final_mixed_compare.asm` to the previous
renderer. The cached guard admits either its unchanged uniform profiles or
initial articulation 127 followed by articulation 63 for all three returning
records. It checks the first six records separately from the last three.
The preceding tempo, geometry, cache-termination, duration, opcode and final
cursor guards remain in force. Unknown simultaneous final profiles fail the
silent rehearsal. Other completion paths can retain preceding behavior without
claiming this qualification.

A positive existing channel-2 counter in the measured third-pattern rest is
set to five pulses for duration 4, or nine for articulation-63 duration 8.
Uniform articulation-127 duration 8 retains fifteen pulses. The code does not
arm a new note, restart timers, discard score pulses or change timestamps.
Final termination uses the existing attack acknowledgement after the old voice
has physically released and its envelope has settled.

The image remains 4089 bytes inside the 4096-byte bound, with SHA-256
`16f78157cb9c49d4d4b22f7776223c77f81b9fd9aad2a64414892cbd508d882d`.
The preceding uniform-return image retains SHA-256
`e220d4f928b3e4fb572d67d630a5691b4aeedd57abf04d5c636bbc3124ee06d9`.
Shared checker extensions require explicit mixed-articulation arguments;
their default uniform contracts remain unchanged.

The physical probe uses its own `gbb-spc-score-final-mixed-v1` schema. It checks
inactive counter/ENVX freezing, real timer release and envelope decay, and a
new final-note attack rather than an unreleased retrigger. It observes owned
terminal PCM for 600000 half-cycles and saves/restores at four points in that
window. Reset and cross-engine continuation preserve raw events, accepted
DSP key/VOL/pitch writes, envelopes and owned PCM. A wrong rest profile,
missing rest rearm or removed inactive freeze fails physical validation.
The preceding image rejects this mixed corpus silently.

All 16 new native/original comparisons pass within the unchanged 4096-cycle
allowance:

| Compared observation | Maximum difference in SPC cycles |
| --- | --- |
| Raw note gate | 2894 |
| Onset interval | 2362 |
| Final stop interval | 2931 |
| Pending pitch-to-stop interval | 36 |
| Final control offset | 139 |

Nine new tests cover the complete owned matrix, silent rejection, physical
faults, metadata forgeries, reset/save/load and bounded synthetic reference
contracts. All 37 score, fixture and scheduler CTest registrations pass.
The new image also passes 200 comparisons against the retained
pending, reverse, peer, following-rest, ready-final, final-peer and uniform
return observations, for 216 native/original comparisons in total.

```sh
cmake -S . -B build-dmg-firmware
cmake --build build-dmg-firmware --target gameboy_sgb_score_final_mixed_probe
ctest --test-dir build-dmg-firmware -R 'native_score_final_mixed$' --output-on-failure
python3 scripts/check_sgb_score_final_mixed_playback.py \
  --probe build-dmg-firmware/gameboy_sgb_score_final_mixed_probe \
  --trace build-dmg-firmware/gameboy_snes_65c816_apu_trace \
  --firmware-dir roms > /tmp/score-final-mixed-reference.json
```

Original-only measurement uses `check_sgb_score_final_mixed_reference.py`.
The playback checker accepts `--reference` to reuse a sanitized artifact with
schema `gbb-score-final-mixed-observation-v1` and verify owned cartridge hashes.
Its result schema is `gbb-score-final-mixed-playback-reference-v1`.
The optional local-reference CTest runs the 16-child matrix. Original children
retain the 8-million-instruction, 180-second, 16 MiB CSV and fewer-than-32768-row
bounds. Native program, source/cache, event, tick, physical-time, PCM and
subprocess limits remain unchanged. Private original instructions, scores,
instrument tables, samples and PCM are not inspected or copied.

This remains an opt-in direct-RAM/IPL-trampoline diagnostic with qualification
and playback false, outside bundled or production selection. The bundled
prototype is unchanged; SGB1/SGB2 program ROMs remain required. Hardware,
original PCM equivalence and real-title replacement qualification remain open.

The subsequent [direct-return observation corpus](sgb-score-direct-return-observations.md)
measures a held voice returning directly with a note, without a preceding rest.
Both originals issue KON while the old voice remains keyed, then give the
returning note its own timer release. Native direct-return implementation and
physical envelope qualification remain the next step.
