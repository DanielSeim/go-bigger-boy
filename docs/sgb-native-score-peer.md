# Experimental clipped-peer voice lifecycle

The subsequent [same-tick end-priority diagnostic](sgb-native-score-order.md)
qualifies channel-2-first skipping and measures the reverse ordering separately.

The isolated native renderer now preserves a clipped peer's voice across pattern
changes. Owned score fixtures executed against both originals confirm that an
inactive channel's gate countdown freezes. A returning note replaces the held
note without an intervening KOF; a returning rest resumes the old countdown.
The preceding [trailing-control diagnostic](sgb-native-score-tail.md) identified
this gap and continues to build independently with its original hash.

## Native behavior and physical evidence

Pattern entry clears the transient KON register and accumulated note mask,
without releasing both voices or canceling their counters. It resets release
cause metadata to scheduler cause zero, preserving the actual KOF state. Startup
still holds both voices before instrument writes. Each timer gate step checks
the current active mask before decrementing its channel's pulse counter.
The existing pending-pulse exclusions, score clock and timer phase are retained.
No extra delays, timer restarts or discarded score pulses are introduced.

When a clipped voice returns with a note, the normal note setup and gate arm
replace its old counter and issue KON. No KOF is fabricated for the outgoing
note. When it returns with a rest, no KON or new gate is armed: the retained
counter becomes active and produces the eventual timer KOF. Short articulation
63 gates still expire before the measured clipping boundary.

`build_sgb_score_peer.py` composes independently written hooks in the existing
owned source. The strict assembler checks operands and placement. The image
remains bounded to 4016 bytes, ending at `$17AF`, with SHA-256
`f0616a5014fdf2f3dc4d6b1ccafcf5192501f4aec0d436cc513a6fa00000c182`.
All preceding image hashes and the bundled prototype remain unchanged.
Source, cache, event, finite-call, tick and measured-profile bounds are retained.

The physical probe checks each inactive half-cycle: a nonzero retained counter
must remain unchanged, its KOF bit must remain clear, and its envelope must stay
positive. Previously released inactive voices retain the existing settled-zero
checks. `frozen_peer_checks` records these observations; baseline, restored and
reset results must match their counts as well as events, DSP edges/writes,
ENVX, owned PCM and cross-engine continuation. The delayed long-gate fixture
observes more than 348000 frozen half-cycles on channel 3.

An outgoing unreleased envelope has `off_half_cycle=0` and an explicit
`retrigger_half_cycle` equal to its replacement KON. Its release-step count and
release-zero flag remain zero/false. This differs from an observed KOF, whose
physical timestamp, ENVX release and decay checks remain mandatory. Validators
accept this lifecycle only with an explicit `clipped_peer=True` option; all
preceding checkers retain their stricter defaults. Retriggers require an earlier
clipped timed event and a qualified pattern-entry onset.

Physical gate comparisons retain raw wall time, including inactivity. For the
internal measured pulse-profile check only, the validator subtracts inactive
pattern intervals derived from actual event timestamps and active masks. The
existing pulse-profile limits remain unchanged. No exported timestamps are
shifted or synthetic KOF edges added.

```sh
python3 scripts/build_sgb_score_peer.py --output /tmp/score-peer.bin
cmake -S . -B build-dmg-firmware
cmake --build build-dmg-firmware --target gameboy_sgb_score_peer_probe
ctest --test-dir build-dmg-firmware -R 'native_score_peer$' --output-on-failure
```

## Opaque reference checks

Four owned 2048-byte fixture banks cover both clipping directions and rest
reactivation. They retain explicit instrument 2, pan, track/song volume and tempo
96 at startup, trailing ED control priority, and eight observed note onsets:

- `clip-2` ends channel 2 first and leaves channel 3's duration-24 note clipped.
  Channel 3 sits out the following pattern and returns with a note at onset 4.
- `clip-3` ends channel 3 first and returns channel 2 with a note at onset 2.
- `rest-2` and `rest-3` insert a duration-8 rest when that clipped peer returns,
  then select duration 16 for its two notes. The remaining long gate expires
  during the rest before the first returning note.

Each case runs with articulation 63 and 127 on SGB1 and SGB2: sixteen fresh
original executions and native comparisons. At articulation 127 the two note
reactivation cases each have 11 real KOF releases and one unreleased retrigger;
all other cases have 12 real releases. Across the matrix, all 188 releases and
four unreleased retriggers match the native lifecycle, voice and onset identities.
Every compared native release comes from the timer, not a substituted scheduler
release. Rest reactivation retains raw gates of approximately 292000 SPC cycles
with the inactive pattern, or 116000 without that intervening inactive pattern.

Maximum native/original raw gate difference is 3460 SPC cycles; maximum onset
interval difference is 2981. The retained 4096-cycle comparison allowances and
2048-cycle intermodel allowance are unchanged. Maximum intermodel onset interval
difference is 56 cycles. Original onset bounds follow the authored spacing:
84000..92000 cycles per 16 score ticks, including the fixtures' longer intervals
when a rest precedes a note. The new diagnostic's bounded gate metadata permits
wall time up to 400000 cycles to represent the measured pause; this does not
change the gate timing tolerance or earlier checkers' metadata limits.

| Case | Articulation | Cartridge SHA-256 |
| --- | --- | --- |
| clip-2 | 127 | `623f6d4c56a1246d3954f00910970e89388e76971a52a5e7d4ac83c84853c8c9` |
| clip-2 | 63 | `b5c6fc44f1aa515fb01c91be7fa9f19f67089d9ebc8d866e62b059ff462ea87f` |
| clip-3 | 127 | `77e3ea8fb546674915c2c1869abeb8b984b6891b1598491b70d8510dc84ec69e` |
| clip-3 | 63 | `9d53678d0282e5622f25b581db65a331050a18a911ea374d16547473e6ce1c85` |
| rest-2 | 127 | `6d040f251d74138c6911e3af951d38cb6822b83d1fc7800b9087da7c8eb2746b` |
| rest-2 | 63 | `a8896254b662d8d6fdb3f638b1e9d00701f82b8ca77fdf8795fc3dc7fe03ddfa` |
| rest-3 | 127 | `61c39f95a5b5996b92eefc577c415ec0e8233584edd9410b65d9c9da75801c88` |
| rest-3 | 63 | `e2b879f805c5434b1314690ecb58827f1db2248148f6f37e2a5b578439bb54a3` |

```sh
python3 scripts/check_sgb_score_peer_reference.py \
  --probe build-dmg-firmware/gameboy_sgb_score_peer_probe \
  --trace build-dmg-firmware/gameboy_snes_65c816_apu_trace \
  --firmware-dir roms > /tmp/score-peer-reference.json
```

Original-only measurement can omit `--probe`. Each original child retains the
8-million-instruction, 180-second, 16 MiB CSV and fewer-than-32768-row bounds.
Only sanitized DSP/timing observations and input hashes are exported. Private
instructions, scores, instrument tables, samples and PCM are not copied.
These local checks require caller-owned originals and do not establish PCM
equivalence. Schemas are `gbb-spc-score-peer-v1` and `gbb-score-peer-reference-v1`.

The 11-test suite covers the fixture matrix, real versus unreleased envelope
termination, two inactive patterns, delayed rest release, short gates, all ten
gate profiles and full 64-event caches, a removed-freeze fault, the preceding
forced-release image, and false envelope/counter/reference metadata. The shared
probe checks physical audio and full lifecycle restoration throughout. All 11
new tests and 27 preceding score/fixture/scheduler suites passed (28 CTest
registrations), along with the bundled-image reproducibility check and
`git diff --check`.

This remains an opt-in diagnostic, with qualification and playback false,
outside bundled or production selection. SGB1/SGB2 program ROMs remain required.
The measured rest cases allow the retained gate to expire before another note;
arbitrary shorter-rest retrigger ordering, later instrument writes on a held
voice, and final termination with a permanently inactive held peer need separate
qualification. Next, measure simultaneous track end/note ordering on both
originals before relaxing that conservative native rejection.
