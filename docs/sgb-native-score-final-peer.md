# Experimental final termination with a sounding peer

The isolated native renderer now reproduces the measured final stop for a
clipped peer, including a voice held through an inactive pattern. Both original
models release that voice with the final KOF FF/00 pulse and issue KON zero.
There is no retrigger and no unresolved voice at the end of the bounded trace.
The preceding [ready final note/rest diagnostic](sgb-native-score-final.md)
continues to build independently with its original hash and held-note behavior.

## Owned fixtures and original observations

`build_sgb_score_final_peer_fixture.py` authors four 2048-byte banks from the
existing owned clipping fixture. Each starts both voices with instrument 2,
pan 10, track volume 127, song volume 160 and tempo 96. Both voices play notes
98 and 99 at ticks 0 and 16. The ending channel retains duration 16; its peer
selects duration 24 for the second note. Trailing ED 64 controls expose actual
volume application separately from logical score state.

| Case | Ending channel | Clipped peer | Final tick | Following pattern |
| --- | --- | --- | --- | --- |
| `clip-2` | 2 | 3 | 32 | None |
| `clip-3` | 3 | 2 | 32 | None |
| `inactive-2` | 2 | 3 | 64 | Solo channel 2 at tick 32 |
| `inactive-3` | 3 | 2 | 64 | Solo channel 3 at tick 32 |

The solo pattern plays notes 9A and 9B with duration 16. Its peer remains
inactive until final termination. Articulations 63 and 127 on SGB1 and SGB2
produce 16 original executions and eight intermodel comparisons.

At articulation 63, every note receives a timer KOF before termination. At
articulation 127, the duration-24 peer remains keyed when its pattern ends.
Immediate termination releases it approximately 84000 SPC cycles after its
last onset. With the solo pattern, its gate stays frozen until final termination
approximately 259500 cycles after that onset. These are actual KOF register
observations, not releases inferred from score durations.

All cases have the same first three final writes:

| Register | Value | Offset from final stop in SPC cycles |
| --- | --- | --- |
| KOF | FF | 0 |
| KOF | 00 | 117 |
| KON | 00 | 156 |

The remainder of the original trace contains repeated zero KOF/KON writes.
The checker preserves their count and rejects subsequent nonzero controls;
it does not export thousands of redundant zero rows or interpret them as
additional releases. Across the matrix there are 80 observed releases:
72 timer releases and eight final-stop releases. No unreleased retrigger or
terminal held voice occurs. Later solo notes apply volume 1/1 only to their
active channel; the inactive peer retains 7/7. Immediate termination retains
7/7 on both voices.

Onset spacing retains the preceding 84000..92000-cycle metadata bound.
The stop occurs before hypothetical following-pattern setup, so its spacing
uses whole timer-pulse bounds of 40..46 pulses per 16 ticks. Raw held-peer
lifetimes use one or three such intervals. These metadata bounds describe the
measured geometry; timing comparisons still require the existing 4096-cycle
native/original and 2048-cycle intermodel allowances.

## Guarded native completion and physical validation

`build_sgb_score_final_peer.py` composes checked hooks over the preceding final
renderer and independently written `score_final_peer*.asm`. It admits the new
completion only at tempo 96, with one or two patterns, an initial paired mask,
the measured initial notes/durations and terminating cache records, and, when
present, the two duration-16 solo notes on the ending channel. Other geometry
keeps preceding completion behavior. The same-tick ready final note/rest path
also keeps its independently validated behavior.

Direct-page BB marks the selected completion phase. Gate counters and pending
pulses are cleared, the final pulse writes KOF FF, KOF zero, then KON zero,
and the score timers stop. The existing `duet_wait` holds KOF across more than
two DSP frames before clearing it. This is the existing DSP latch safeguard;
no delay was calibrated to the original's 117-cycle offset. The selected image
is 4089 bytes with SHA-256
`09c6b3f1b641cd232f936a68b507be239a789766e7b7ec60f606556710db36e5`.
All preceding image hashes and the bundled prototype remain unchanged.

The earlier probe incorrectly required an inactive voice with a newly cleared
counter to be already released while completion was still preparing its KOF.
The new probe observes that transition under the explicit BB phase flag.
It retains per-half-cycle counter freeze and positive-envelope checks before
completion, records the actual accepted FF/00 writes and their physical KOF
edge, and checks ADSR release and both ENVX registers settling to zero after
the pulse clears. It also requires quiet final PCM. Save/load and cross-engine
checks cover entry into completion, the actual release and the terminal tail;
baseline, reset and restored full results must agree.

The legacy voice validator receives a copy projecting the stop edge's raw FF
held register onto voices 2/3. The new checker separately binds the unchanged
raw register and every KON/KOF edge to accepted writes. No timestamps, masks,
gate causes or source records are shifted or synthesized. Final-stop releases
require scheduler cause zero at the final tick; all preceding timer releases
retain cause one. A removed KOF, omitted DSP latch hold or removed inactive
freeze must fail physical validation.

All 16 native/original comparisons pass. Maximum timing differences in SPC
cycles are:

| Compared observation | Maximum difference |
| --- | --- |
| Raw note gate | 2834 |
| Onset interval | 989 |
| Final stop interval | 2444 |
| Final control write offset | 80 |

All nine new tests and the preceding score/fixture/scheduler suites passed
(35 CTest registrations). The new image also passed 152 comparisons against
retained pending, reverse, peer, following-rest and ready-final observations,
for 168 native/original comparisons including this milestone's 16 cases.
The bundled-image reproducibility check and `git diff --check` also passed.

```sh
cmake -S . -B build-dmg-firmware
cmake --build build-dmg-firmware --target gameboy_sgb_score_final_peer_probe
ctest --test-dir build-dmg-firmware -R 'native_score_final_peer$' --output-on-failure
python3 scripts/check_sgb_score_final_peer_playback.py \
  --probe build-dmg-firmware/gameboy_sgb_score_final_peer_probe \
  --trace build-dmg-firmware/gameboy_snes_65c816_apu_trace \
  --firmware-dir roms > /tmp/score-final-peer-reference.json
```

Original-only measurements use `check_sgb_score_final_peer_reference.py`.
The playback checker can use `--reference /tmp/score-final-peer-observations.json`
to reuse unchanged sanitized observations and verify owned cartridge hashes.
Schemas are `gbb-score-final-peer-observation-v1`, `gbb-spc-score-final-peer-v1`
and `gbb-score-final-peer-playback-reference-v1`.
Each original child retains the 8-million-instruction, 180-second, 16 MiB CSV
and fewer-than-32768-row limits. Native program, source, cache, event, tick,
physical-time, PCM and subprocess bounds remain unchanged. Private instructions,
scores, instrument tables, samples and PCM are not inspected or copied.

This remains an opt-in direct-RAM/IPL-trampoline diagnostic, with qualification
and playback false, outside bundled and production selection. SGB1/SGB2 program
ROMs remain required. The authored cases have no ready boundary event, so they
do not qualify retriggering a held peer at the final tick. Original PCM
equivalence, hardware and real-title replacement qualification remain open.

The subsequent [returning final note/rest diagnostic](sgb-native-score-final-return.md)
measures channel 2 returning through a rest before channel 3's final end.
That rest releases the old held voice before the final pulse; a pending note
then starts a new held attack. Mixed articulation and an immediate returning
note without that rest remain separate measurements.
