# Pending notes across an inactive pattern

Owned cartridges executed against both originals establish what happens when
channel 2 reaches a note or rest before a same-tick channel-3 end, then becomes
inactive in the next pattern. This is an observation milestone following the
[reverse-order renderer](sgb-native-score-reverse.md). The new cases remain
unsupported by that renderer and reject before live playback.

The subsequent [pending-note renderer](sgb-native-score-pending.md) implements
these measured cases in a separate opt-in diagnostic; this document and its
checker retain the earlier image's rejection contract.

## Fixture and transport

`build_sgb_score_pending_fixture.py` uses an independently authored 2048-byte
bank and the existing JOYP/SOU_TRN cartridge transport. The four-pattern layout
has active masks 12, 8, 4 and 12. Initial voices 2/3 select instrument 2, center
pan, track volume 127, song volume 160 and tempo 96. At tick 32 channel 2 executes
ED64, a duration/articulation pair and note A0 or rest C9 before channel 3 ends
with its own ED64. Channel 3 alone plays notes 9A/9B in the next pattern.
Channel 2 returns in pattern 2, then both channels play the final pattern.

The matrix crosses pending duration 8/16, articulation 63/127 and SGB1/SGB2:

| Case | Boundary channel-2 event | Channel-2 return |
| --- | --- | --- |
| note-return | A0 | Two duration-16 notes |
| rest-return | A0 | Duration-8 rest, then two duration-16 notes |
| long-rest-return | A0 | Duration-16 rest, then two duration-16 notes |
| pending-rest | C9 | Two duration-16 notes |

These are 16 distinct owned cartridges and 32 opaque original executions. The
first inactive pattern lasts 32 score ticks, longer than the normal release
profiles of every pending note in this matrix. Onset ticks are
0/16/32/48/64/80/96/112 for direct note returns;
0/16/32/48/72/88/104/120 for duration-8 rest returns; and
0/16/32/48/80/96/112/128 for duration-16 rest returns.

## Physical key edges and mix

For a pending note, KON at tick 32 includes both voices (mask 12), even though
channel 2 has no track pointer in the new pattern. Voice 2 starts at pitch 1700,
while channel 3 starts at 1200. A pending rest adds neither a pitch write nor a
channel-2 KON: only voice 3 starts (mask 8). Each case has eight actual KON groups.

The note retains channel 2's previous DSP volume 7/7 at this KON. Its ED64 has
already changed logical inherited state, but the DSP does not receive volume
1/1 during the inactive pattern. A returning rest also leaves the DSP volume
unchanged. The actual channel-2 volume update occurs only immediately before
its first returning note, which starts at volume 1/1. The observer exports the
accepted VOL-L/VOL-R pair change and its physical interval from the pending KON;
that update must lie within 4096 SPC cycles before the returning note's KON.
This distinguishes logical mix inheritance from the currently installed DSP mix.

In `note-return`, no KOF terminates the pending note during inactivity. Its first
returning note replaces it with another KON on channel 2, with no intervening
KOF. The ledger records this as an unreleased retrigger from onset 2 to onset 4,
rather than inventing a release for the outgoing note. Pending durations 8 and
16 both survive the full inactive pattern at both articulations.

Returning rests produce an actual pending-note KOF before the first returning
note. The measured duration-8 rest releases at the same time for pending durations
8 and 16: approximately 192624 SPC cycles after the pending KON at articulation
63, or 204915 at articulation 127 on SGB1. A duration-16 rest releases at approximately 221300 cycles for articulation 63,
or 248382 for articulation 127, also independently of pending duration.

| Returning rest duration | Articulation | Pending-to-KOF SPC cycles, SGB1 |
| --- | --- | --- |
| 8 | 63 | 192624 |
| 8 | 127 | 204915 |
| 16 | 63 | 221300 |
| 16 | 127 | 248382 |

These values apply to both pending durations in each row. This prevents interpreting every returning rest as simply
resuming an unchanged old gate counter. No internal original counter is inspected;
the evidence specifies observable key-edge and mix behavior.

The earlier [clipped-peer experiment](sgb-native-score-peer.md) used particular
rest cases where preserving its native frozen counter matched reference release
timing within the retained allowance. Those cases remain validated, but do not
establish a general original countdown-resume rule for arbitrary returning rests.

## Reproducible checks and evidence limits

```sh
cmake -S . -B build-dmg-firmware
ctest --test-dir build-dmg-firmware \
  -R 'native_score_pending_observation$' --output-on-failure
python3 scripts/check_sgb_score_pending_reference.py \
  --probe build-dmg-firmware/gameboy_sgb_score_reverse_probe \
  --trace build-dmg-firmware/gameboy_snes_65c816_apu_trace \
  --firmware-dir roms > /tmp/score-pending-observations.json
```

The report schema is `gbb-score-pending-observation-v1`, with qualification and
playback false and `native_supported: false`. Original-only measurement can omit
`--probe`. The complete reference matrix checks unique case/profile/model
identities, exact onset setup, actual KOF identities, explicitly unreleased
retriggers, pitch-write order and timing, and deferred volume application.
The original models must agree within the existing 2048-SPC-cycle allowance for
onset intervals, raw gate durations, retrigger intervals and pitch/volume timing.
The matrix contains 400 directly observed KOF releases and eight unreleased
retriggers. Maximum intermodel differences are 56 cycles for onset intervals,
49 for raw gates, 46 for retriggers, zero for boundary pitch timing, and 50 for
volume application. No native/original playback comparison or original PCM
equivalence is claimed.

Each original child retains the 8-million-instruction, 180-second, 16 MiB CSV
and fewer-than-32768-row bounds. Only sanitized owned-voice DSP metadata and input
hashes are exported. Original firmware instructions, scores, instrument tables,
samples and PCM are not inspected or copied. Delayed pending-note releases have
an explicit 400000-cycle metadata bound; ordinary gate metadata remains bounded
to 200000. This is not an increased comparison tolerance. No timestamps are
shifted, missing KOF fabricated or original capture retained in the repository.

The optional native probe verifies all 16 banks still reject silently with E2,
zero live records, instrument/voice writes, key edges and nonzero PCM. Reset,
restore and source/cache guards remain intact. The reverse image retains SHA-256
`deece9488c7138fb4e9f0cde22beccf2edd3770f42b1df2a257f9b387cd15dda`;
the bundled prototype and production firmware selection are unchanged.
SGB1/SGB2 program ROMs remain required.

Nine new tests cover owned layout and symbolic boundary execution, source and
image reproducibility, continued native rejection, pending note/rest KON masks,
release/retrigger ledgers, deferred mix, complete intermodel identities and false
metadata, malformed bounded traces and invalid inputs. The new suite and 30
preceding score, fixture and scheduler registrations passed (31 CTest
registrations total), along with bundled-image reproducibility and
`git diff --check`.

Next, implement the measured pending-note transition: retain its KON bit into the
inactive pattern's combined KON, preserve the installed DSP mix while carrying
logical ED state, and handle note versus rest reactivation using the qualified
release profiles. Validate actual writes, key edges, envelopes, inactive state
and save/restore before relaxing the current guard. Following-pattern rests,
final termination with a pending voice, broader rest timing and later instrument
writes remain separate qualification work.
