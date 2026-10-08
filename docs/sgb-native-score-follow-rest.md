# Experimental boundary notes through an immediate following rest

The independent diagnostic now keys a ready channel-2 boundary note even when
the immediately following pattern starts channel 2 with a rest. Both originals
confirm that the rest replaces the pending note's gate duration and articulation,
while the pending pitch and previous actual DSP volume survive into KON. A
boundary rest produces no channel-2 pitch write or KON. This extends the
[pending-note diagnostic](sgb-native-score-pending.md) in a separate image;
earlier independently buildable images and their rejection contracts remain intact.

The subsequent [final-boundary diagnostic](sgb-native-score-final.md) implements
the separately measured single-pattern final note/rest case in a new image.

## Owned measurements

`build_sgb_score_follow_rest_fixture.py` creates 32 independently authored
2048-byte banks, each transported by the existing JOYP/SOU_TRN cartridge builder.
Four patterns have active masks 12, 12, 4 and 12. Voices 2/3 initially select
instrument 2, centered pan, track volume 127, song volume 160 and tempo 96.
At tick 32, channel 2 executes ED64, duration 8/16, articulation 63/127 and
note A0 or rest C9 before channel 3 ends with ED64. The following pattern starts
channel 2 with a duration-8/16 rest, articulation 63/127, then note 9C of duration
16. Channel 3 plays note 9A for the rest's duration, then note 9B of duration 16;
both tracks finish together. Channel 2 alone then plays 9D/9E, and both voices
play the final A3/A4 pattern.

The matrix crosses both boundary kinds, boundary duration 8/16, following-rest
duration 8/16, boundary articulation 63/127 and rest articulation 63/127. Each
owned cartridge runs on SGB1 and SGB2, for 64 fresh original measurements.
There are eight KON groups and 14 actual note releases for a boundary note,
or 13 for a boundary rest: 864 total KOF releases, with no unreleased retriggers.
Onset ticks are 0/16/32/40/56/72/88/104 for a duration-8 following rest, or
0/16/32/48/64/80/96/112 for a duration-16 following rest.

At tick 32 the boundary-note case has KON mask 12 and voice-2 pitch 1700;
voice 3 starts at pitch 1200. Voice 2 retains DSP volume 7/7, despite logical
ED64 having selected volume 1/1. The following rest writes neither pitch nor
volume. The first subsequent note 9C applies volume 1/1 immediately before its
KON. A boundary rest gives only voice-3 KON (mask 8) and pitch 1200 at tick 32.

Crossing durations and articulations rules out retaining the boundary note's
own gate profile. SGB1 pending-to-KOF intervals, in SPC cycles:

| Following-rest duration | Following-rest articulation | Observed interval |
| --- | --- | ---: |
| 8 | 63 | 18163 |
| 8 | 127 | 28405..28413 |
| 16 | 63 | 45243..45251 |
| 16 | 127 | 73465 |

These intervals are independent of boundary duration and articulation within
the retained 2048-cycle comparison allowance (maximum observed variation is
eight cycles). Release occurs during the rest,
before note 9C. Maximum intermodel differences are 56 cycles for onset intervals,
57 for gates, zero for boundary pitch timing and 55 for the delayed volume update.

## Native execution

`scripts/build_sgb_score_follow_rest.py` composes checked hooks over the pending
renderer and independently authored `firmware/sgb/score_follow_rest.asm`.
The new guard requires tempo 96, a following pattern with both voices active,
an initial channel-2 rest of duration 8/16, and no instrument prefix on that rest.
The existing boundary guard still requires duration 8/16 and no boundary
instrument prefix. Silent bounded rehearsal checks the complete bank before
any live writes. Unknown profiles, solo following patterns, held-voice instrument
writes and final ready-event termination remain excluded from this new path.

The pending KON bit survives normal pattern-entry clearing. The first rest uses
its measured profile and additionally preserves that profile in the pending
counter slot used at KON. Otherwise its cached zero gate would overwrite the
correct counter as the surviving boundary note is keyed. The rest's raw record
remains C9 with logical volumes 0/0; it is not rewritten into a note. The physical
KON binds the previous pattern's note and the current pattern's channel-3 note,
and counts each affected voice once despite two executed channel-2 records at
tick 32. Gate termination remains a real timer KOF.

The DSP retains the boundary note's pitch and old volume until the first later
note applies its actual volume writes. Two private DP fields hold the immediate
rest mode and selected rest profile; both are reset at startup and cleared after
pattern-entry KON. No timer is restarted, score pulse discarded, timestamp
shifted or delay introduced to fit the measurements.

The image is 4056 bytes. SHA-256:
`5914bedd6cb346464b442d8ec6f0af3df5e19c6f41d957bcf0d35331f2162d53`.
The existing 4096-byte program, 2048-byte source, four-pattern,
eight-events-per-track, 64-event, 2032-tick, 30-million-half-cycle,
500000-PCM-frame and 60-second native-child bounds are unchanged.

## Validation and reproduction

The checker validates the exact owned corpus against independent symbolic
execution, including the old-pattern boundary event and new-pattern rest at the
same tick. Actual accepted DSP writes are checked separately from logical state.
It checks KON grouping, pitches, live volumes, pending profiles, held KOF masks,
release identities and timer cause, owned ADSR trajectories, source/cache guards,
PCM bounds, quiet completion, reset and same/cross-engine save/load equality.
Earlier shared metadata checks are reused without changing preceding contracts.

All 64 native/original comparisons pass. Maximum native/original differences,
in SPC cycles, are 3487 for gates, 2882 for onset intervals, 1053 for boundary
pitch-to-KON timing and 2178 for delayed volume timing. The retained allowance
is 4096 for each native/original measurement and 2048 for intermodel agreement.
The new image also passes all 72 preceding comparisons against unchanged saved
inputs and observations: 32 pending-note, 24 reverse-order and 16 clipped-peer.

```sh
cmake -S . -B build-dmg-firmware
cmake --build build-dmg-firmware --target gameboy_sgb_score_follow_rest_probe
ctest --test-dir build-dmg-firmware -R 'native_score_follow_rest$' --output-on-failure
python3 scripts/check_sgb_score_follow_rest_playback.py \
  --probe build-dmg-firmware/gameboy_sgb_score_follow_rest_probe \
  --trace build-dmg-firmware/gameboy_snes_65c816_apu_trace \
  --firmware-dir roms > /tmp/score-follow-rest-reference.json
```

Original-only measurements use `check_sgb_score_follow_rest_reference.py` with
`--trace` and `--firmware-dir`. For unchanged sanitized observations, the playback
checker accepts `--reference /tmp/score-follow-rest-observations.json` instead;
it checks each owned cartridge's SHA-256 before comparing. Every original child
retains the 8-million-instruction, 180-second, 16 MiB CSV and fewer-than-32768-row
bounds. Only owned-voice DSP/register/timing metadata and input hashes are exported;
private instructions, scores, instrument tables, samples and PCM are not inspected
or copied. Schemas are `gbb-score-follow-rest-observation-v1`,
`gbb-spc-score-follow-rest-v1` and `gbb-score-follow-rest-playback-reference-v1`.

All seven new tests and 32 preceding score, fixture and scheduler suites passed
(33 CTest registrations). Tests cover crossed profiles, raw rest records versus
actual pending KON, delayed volume writes, silent conservative rejection, a
removed rest-profile hook, and false raw/physical/envelope/reference metadata.
The bundled prototype reproducibility check retains SHA-256
`8222797ddeec5681af61fda28c6afc241898887cd0f254ce2f691f30d4b13482`;
`git diff --check` also passed.

This remains an opt-in direct-RAM/IPL-trampoline diagnostic with qualification
and playback false, outside bundled and production selection. SGB1/SGB2 program
ROMs remain required. Hardware checks, original PCM equivalence, broader driver
commands and real-title replacement qualification remain open.

Next, measure a final channel-3 end with a ready channel-2 note/rest and no
following pattern. Establish whether the event executes, receives KON, and how
final KOF/volume/pitch writes occur before relaxing the final-boundary guard.
