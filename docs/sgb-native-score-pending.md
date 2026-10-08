# Experimental pending notes across an inactive pattern

The independent native diagnostic implements the lifecycle measured in
[pending-note observations](sgb-score-pending-observations.md): a channel-2 note
executed before a same-tick channel-3 end joins the following pattern's KON,
even when channel 2 is absent from that pattern. Its pitch changes immediately,
its logical controls carry forward, and its actual DSP volume remains unchanged
until a returning note. A returning rest controls the held note's release.

## Execution and limits

`scripts/build_sgb_score_pending.py` layers checked source hooks over the
[reverse-order diagnostic](sgb-native-score-reverse.md), preserving that image's
SHA-256 and earlier guards. Three explicitly initialized private DP fields hold
boundary mix suppression, the pending KON bit, and the returning-event state.
Only that pending bit survives the next pattern's normal transient KON clear;
ordinary pattern entries do not carry stale KON bits. The affected mask includes
the pending note as well as the newly active voice. A boundary rest adds no bit.

The boundary note logs logical volume 1/1 after ED64 and writes pitch 1700,
but skips VOL-L/VOL-R writes. The actual combined KON retains channel 2's prior
7/7 volume. Existing inactive-mask checks freeze its armed gate counter while
channel 3 executes its two notes. The first returning note writes volume 1/1
and retriggers without an intervening KOF. Its prior envelope terminates with a
retrigger timestamp, not a fabricated release.

If the first returning event is a rest, the runtime looks up that rest's own
measured duration/articulation profile and arms the existing channel-2 counter
without pitch, volume or KON writes. This makes the release independent of the
pending note's duration. The original free-running timer phase and queued score
pulses remain intact; no timestamps are adjusted or delays inserted.

The newly admitted transition requires tempo 96, boundary duration 8/16, one
inactive pattern followed by a channel-2-only pattern, and no instrument prefix
on the boundary or returning held voice. Returning rests require duration 8/16
and a measured articulation profile. These guards run during bounded silent
rehearsal before live playback. A next pattern with channel 2 active and an
initial rest, a final same-tick stop, permanently inactive pending voices, longer
inactive chains, other tempos, unmeasured rest durations and held-voice instrument
writes remain excluded. The preceding immediate note-overwrite path is retained.

Program size is 4053 bytes, below the existing 4096-byte diagnostic limit.
SHA-256: `c55ef2d6f97b469cf42bd56efbe1e519da9088350bd4b1ad17111ee9b242e117`.
The 2048-byte source, four-pattern, eight-events-per-track, 64-event,
2032-tick, 30-million-half-cycle and 500000-PCM-frame bounds are unchanged.

## Validation

The playback checker qualifies the exact 16 owned banks from the observation
milestone: four cases, pending durations 8/16, and articulations 63/127. It binds
every raw record to the independent symbolic execution order, including the
old-pattern boundary event. It checks logical control inheritance separately
from actual accepted DSP volume/pitch writes and physical KON/KOF edges. No
record is dropped, reassigned to an active pattern, or given a shifted timestamp.
The shared probe checks frozen counters and sounding envelopes, source/cache
guards, silence after completion, reset equality, same/cross-engine save/load,
and restoration following actual pitch writes.

All 32 native comparisons pass against the previously measured, unchanged
SGB1/SGB2 inputs: 400 real KOF releases and eight unreleased retriggers. The saved
reference includes each owned cartridge's SHA-256, which is checked before
comparison. No new original execution was needed for unchanged inputs. Maximum
native/original differences, in SPC cycles:

| Observation | Maximum absolute difference |
| --- | ---: |
| Gate duration | 3466 |
| Onset interval | 2907 |
| Boundary pitch-to-KON interval | 1419 |
| Pending-to-retrigger interval | 1612 |
| Delayed volume update interval | 1849 |

The retained allowance is 4096 cycles for each native/original comparison and
2048 for intermodel agreement. The new image also passes all 24 comparisons of
the preceding reverse-order corpus and all 16 preceding clipped-peer comparisons
against their saved independent observations.
The original observation checker continues to test the earlier image's silent
rejection; it cannot claim support for this new renderer.

```sh
cmake -S . -B build-dmg-firmware
cmake --build build-dmg-firmware --target gameboy_sgb_score_pending_probe
ctest --test-dir build-dmg-firmware -R 'native_score_pending$' --output-on-failure
python3 scripts/check_sgb_score_pending_playback.py \
  --probe build-dmg-firmware/gameboy_sgb_score_pending_probe \
  --trace build-dmg-firmware/gameboy_snes_65c816_apu_trace \
  --firmware-dir roms > /tmp/score-pending-playback.json
```

To reuse an unchanged sanitized observation report, replace `--trace` and
`--firmware-dir` with `--reference /tmp/score-pending-observations.json`.
Fresh original runs retain the 8-million-instruction, 180-second, 16 MiB CSV
and fewer-than-32768-row bounds. Original instructions, scores, instrument
tables, samples and PCM are not inspected or copied. Schemas are
`gbb-spc-score-pending-v1` and `gbb-score-pending-playback-reference-v1`.

All nine new playback tests and 31 preceding score, fixture and scheduler suites
passed (32 CTest registrations). Coverage includes silent unknown-profile and
held-voice-prefix rejection, prior boundary cases, false logical/physical/envelope
metadata, and deliberately removed pending-KON and rest-rearm hooks. The bundled
prototype reproducibility check retains SHA-256
`8222797ddeec5681af61fda28c6afc241898887cd0f254ce2f691f30d4b13482`;
`git diff --check` also passed.

This is an opt-in direct-RAM/IPL-trampoline diagnostic. Qualification and
playback remain false, and it is outside bundled and production selection.
SGB1/SGB2 program ROMs remain required. Original PCM equivalence, hardware
validation, broader driver commands and real-title replacement qualification
remain open.

Next, measure a ready channel-2 note/rest before a channel-3 end when the
immediately following pattern starts channel 2 with a rest. Determine which
pitch, volume, KON mask and gate profile apply before implementing that guard
extension. This differs from the already measured returning rest after an
inactive pattern.
