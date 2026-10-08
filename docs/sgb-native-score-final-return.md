# Experimental final note/rest after a voice returns from inactivity

The isolated renderer now reproduces the measured pending final note/rest after
channel 2 returns from inactivity. Its returning rest first releases the old
held note through a timer KOF. Final termination then writes KOF FF/00 and keys
the pending note, or writes KON zero for the pending rest. The note remains
unreleased throughout the bounded post-stop observation; the rest stays silent.
The preceding [final clipped-peer diagnostic](sgb-native-score-final-peer.md)
continues to build independently with its original hash.

## Owned fixtures and opaque observations

`build_sgb_score_final_return_fixture.py` authors three-pattern 2048-byte banks:

| Pattern | Start tick | Channels | Owned events |
| --- | --- | --- | --- |
| 0 | 0 | 2, 3 | Notes 98/99; second duration 24 on channel 2, 16 on channel 3 |
| 1 | 32 | 3 | Two duration-16 notes 9A/9B; channel 2 remains inactive |
| 2 | 64 | 2, 3 | Both start with a duration-4 or duration-8 rest |

At tick 68 or 72, channel 2 executes ED 64 and a pending duration-8/16 note A0
or rest C9. Channel 3 executes ED 64 and ends on the same tick. The phrase list
terminates there. Startup retains instrument 2, pan 10, track volume 127, song
volume 160 and tempo 96. Channel 3's earlier ED 64 gives its solo notes actual
volume 1/1; channel 2 retains actual volume 7/7 through its inactivity and rests.
Both articulations 63/127, both returning-rest durations, both pending durations,
both pending event kinds and SGB1/SGB2 produce 32 original executions.

At articulation 63, channel 2's duration-24 note releases before it becomes
inactive. Its returning rest cannot create a new note or unbound KOF. At
articulation 127, the old voice stays keyed while inactive, then the returning
rest changes its release timing. That release is governed by the new rest,
rather than merely resuming the old duration-24 countdown. Relative to the
preceding channel-3 onset at tick 48, its observed KOF occurs at:

| Returning rest | Original release interval in SPC cycles | Native rest pulse profile |
| --- | --- | --- |
| Duration 4 | 96833..96841 | 5 |
| Duration 8 | 119361..119369 | 15 |

The interval includes the 16 ticks leading to the returning rest. The checker
binds this interval to the raw old-note gate and actual onset intervals; it
does not invent an original timestamp for the rest. The duration-8 profile is
the existing independently measured profile. Duration 4 is qualified here only
as a rest acting on an already keyed articulation-127 voice. Duration-4 notes
and a live voice returning with articulation 63 remain unqualified.

Every original case releases the old voice before final termination. The
final pulse is KOF FF at offset 0, KOF zero at 117 SPC cycles and KON 4 for a
pending note or zero for a pending rest at 156 cycles. The note writes pitch
1700 before the pulse, retains actual volume 7/7, and starts a new attack after
the old voice's real release. The deferred ED control does not update its
actual DSP volume. Neither returning rest writes pitch or volume or issues KON.

Across the matrix, all 192 preceding note releases are actual timer KOFs.
There are no unreleased retriggers and no voice pending at the final FF write.
The 16 pending final notes have no later observed KOF; all 16 pending rests
finish without an unresolved voice. Post-stop observation windows span
2737196..4511800 SPC cycles. This establishes a bounded register-write lifecycle,
not indefinite sound or original PCM/envelope equivalence. Maximum intermodel
differences are 49 cycles for gates, 48 for onset intervals, 7 for stop intervals
and zero for pitch/control offsets, within the retained 2048-cycle allowance.

## Native implementation and physical checks

`build_sgb_score_final_return.py` composes checked hooks over the preceding
renderer and independently written `score_final_return*.asm`. A cached-profile
guard marks direct-page BE only for tempo 96, three patterns with masks 12/8/12,
the measured initial duration/clipping topology, two solo duration-16 events,
matching duration-4/8 returning rests, a pending duration-8/16 A0/C9 event,
the bounded terminating cache records, and matching articulation throughout.
The first-end final guard separately requires the expected channel cursors.
The existing boundary-prefix guard still rejects instrument reselection there.
The same-tick ready event remains the final raw record at the final score tick.

For this profile, a channel-2 rest in the third pattern changes its positive
existing pulse counter to 5 or 15. It does not arm a new note, set pending KON,
or restart the timer. If the old note has already released, the counter remains
zero. At completion, the existing final-ready path performs the FF/00 pulse,
keys the pending note and waits for owned DSP attack visibility before clearing
KON. The old voice has settled before this point, so it cannot falsely satisfy
that attack acknowledgement. The pending note has no final timer KOF.

The image remains 4089 bytes with SHA-256
`e220d4f928b3e4fb572d67d630a5691b4aeedd57abf04d5c636bbc3124ee06d9`.
The code uses existing gaps inside the 4096-byte diagnostic bound. The original
ten note profiles, previous image hashes and bundled prototype are unchanged.
Other scores retain their preceding behavior; this checker qualifies only the
owned returning-rest/final-ready corpus. Unknown simultaneous final profiles
fail the silent rehearsal. Non-simultaneous ends can retain preceding completion
without claiming this new qualification.

The physical probe observes the inactive counter and positive ENVX every half
cycle, records the returning timer release and real envelope decay to zero,
and distinguishes the final note's new attack from an unreleased retrigger.
It observes terminal note/rest PCM for 600000 half-cycles, beyond every admitted
gate, and saves/restores at four points in that window. Actual accepted key and
VOL/pitch writes must bind to the raw events and physical edges. Reset, save/load
and cross-engine continuation must preserve full events, DSP writes, envelope
trajectories and owned PCM. A missing rest rearm, removed inactive freeze or
discarded final KON must fail.

All 32 native/original comparisons pass with the retained 4096-cycle allowance:

| Compared observation | Maximum difference in SPC cycles |
| --- | --- |
| Raw note gate | 2894 |
| Onset interval | 2365 |
| Final stop interval | 2942 |
| Pending pitch-to-stop interval | 36 |
| Final control write offset | 139 |

No timestamps are shifted, score pulses discarded or fitting delays added.
The local native returning-rest profile check uses its actual raw rest event;
the exported gate and original comparison retain the full inactivity interval.

All ten new tests and the preceding score/fixture/scheduler suites passed
(36 CTest registrations). The new image also passed 168 comparisons against
retained pending, reverse, peer, following-rest, ready-final and final-peer
observations, for 200 native/original comparisons including the 32 new cases.
The bundled-image reproducibility check and `git diff --check` passed.

```sh
cmake -S . -B build-dmg-firmware
cmake --build build-dmg-firmware --target gameboy_sgb_score_final_return_probe
ctest --test-dir build-dmg-firmware -R 'native_score_final_return$' --output-on-failure
python3 scripts/check_sgb_score_final_return_playback.py \
  --probe build-dmg-firmware/gameboy_sgb_score_final_return_probe \
  --trace build-dmg-firmware/gameboy_snes_65c816_apu_trace \
  --firmware-dir roms > /tmp/score-final-return-reference.json
```

Original-only measurement uses `check_sgb_score_final_return_reference.py`.
The playback checker accepts `--reference /tmp/score-final-return-observations.json`
to reuse unchanged sanitized observations and verify owned cartridge hashes.
Schemas are `gbb-score-final-return-observation-v1`, `gbb-spc-score-final-return-v1`
and `gbb-score-final-return-playback-reference-v1`.
Original children retain the 8-million-instruction, 180-second, 16 MiB CSV and
fewer-than-32768-row bounds. Native source, program, cache, event, tick,
physical-time, PCM and subprocess limits remain unchanged. The longer CTest
timeout covers the 32-child matrix, without changing any individual execution
cap. Private instructions, scores, instrument tables, samples and PCM are not
inspected or copied.

This remains an opt-in direct-RAM/IPL-trampoline diagnostic with qualification
and playback false, outside bundled or production selection. SGB1/SGB2 program
ROMs remain required. Hardware, original PCM equivalence and real-title firmware
replacement qualification remain open.

Next, measure mixed articulation when a voice held at articulation 127 returns
with an articulation-63 rest, especially duration 4. The current uniform-63
cases release before inactivity and cannot validate that live-voice profile.
Use both originals before extending the matching-articulation guard.
