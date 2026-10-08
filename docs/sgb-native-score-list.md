# Experimental bounded phrase lists

The isolated score renderer now accepts one to four patterns, replacing the
exactly-two-pattern restriction of [multi-page score banks](sgb-native-score-bank.md).
Each pattern still uses channels 2/3 with one to eight expanded events per track.
The next diagnostic permits [solo tracks and sparse transitions](sgb-native-score-sparse.md).
The complete phrase may contain up to 64 events; its total duration remains
bounded to 2032 ticks. Both expanded tracks must be nonempty in every pattern.
A zero table pointer terminates the phrase after its last pattern. Empty lists,
a fifth nonzero pointer and incomplete words reject before rendering.

This driver remains an experimental direct-RAM diagnostic, outside bundling and
production selection. SGB1/SGB2 program ROMs remain required. More patterns
remove a grammar limitation; they do not qualify real-title playback, arbitrary
instruments or a firmware-free whole-system replacement.

## Native bounds and ownership

The parser walks the bounded 16-bit phrase pointer until its terminator and
validates every pattern before live playback. Eight 32-byte track slots hold
the expanded tracks. Runtime pattern transitions use the validated count,
advancing the cache base by 64 bytes per pattern. A silent scheduling rehearsal
checks first-end clipping, same-tick end/peer-event ambiguity and the global
tick budget before DSP setup, timer start or the first live event. A 2033-tick
phrase rejects even if its early patterns contain otherwise valid notes.

| Region | Owner |
| --- | --- |
| `$0800..143A` | Owned diagnostic code, tables, directory/sample and padding |
| `$2B00..32FF` | Immutable source bank, up to 2048 bytes |
| `$4000..413F` | At most 320 bytes of five-byte event records |
| `$4200` page | Eight expanded duration/opcode track slots |
| `$4300/$4400` pages | Gate pulses and articulation at each pair offset |
| `$4500/$4600` pages | Per-event left/right volume |
| `$4700/$4800/$4900` pages | Per-event pan/track/song volume |

The event-log count uses low byte `$28` and high byte `$9B`. `$A1` marks a record
in progress; the probe observes its count only after all five bytes are written.
This prevents the transient zero count during the 255-to-256 carry from being
mistaken for a published record. `$9C` identifies the current pattern, and
`$9D/$9E` count rehearsal ticks. The source bounds, 8192-byte read budget and
private diagnostic length inputs remain as described in the bank milestone.
Source and surrounding cache guards must survive render, quiet tail, reset and
restore unchanged. These direct-page locations are not a production mailbox API.

Duration and articulation start unset for each track. Pan/track volume reset to
10/127 at every track/pattern; shared song volume starts at 160 and persists
through the phrase. E5 remains restricted to the first pattern's channel-2
prefix. Later patterns must explicitly establish any settings they need.
One nonnested EF call per track, counts 1..3, read-only shared bodies, inherited
callee/repeat/return controls, at most 32 executed controls per track, measured
mix points, instrument 2, octave notes 24..36 and the ten qualified gate profiles
retain their preceding contracts. Unsupported data rejects without live events,
KON or PCM. First-end clipping remains qualified; ambiguous same-tick peer events
remain rejected.

`firmware/sgb/score_list.asm` implements the bounded list parser;
`score_list_runtime.asm` adds tick accounting and the two-page event log.
`build_sgb_score_list.py` composes the prior independently written source and
relocates the parallel caches. The strict assembler checks overlaps and operand
bounds. The reproducible 3131-byte image has SHA-256
`b3b3441f7d2c32b83c042373d17226f7d81d55616a56c0b4590184f411880531`.
Prior images and the owned directory/BRR sample bytes remain unchanged.

```sh
python3 scripts/build_sgb_score_list.py --output /tmp/score-list.bin
cmake -S . -B build-dmg-firmware
cmake --build build-dmg-firmware --target gameboy_sgb_score_list_probe
ctest --test-dir build-dmg-firmware -R 'native_score_list$' --output-on-failure
```

## Owned and private evidence

The 25 ROM-free tests rerun the preceding bank/mix/envelope contracts, with the
one-pattern terminator assertion updated for the new grammar. New cases cover
all four pattern counts, repeated table pointers, 64-event logs under both
articulations, all octave pitches, the exact 2032-tick boundary, silent overflow
rejection, malformed later patterns, truncated phrase words, control resets,
persisting song volume and clipping/ambiguity at later pattern boundaries.
The independent raw scheduler checks each event and every pattern start.
Typed report guards reject invalid pattern entries and altered reference matrices.

The physical APU probe compares complete baseline, restored and reset results,
including DSP edges, ENVX summaries and owned PCM hashes. It also checks
cross-engine restore during a record at counts 255 and 256 and during the
intermediate low-zero/high-zero carry, then resumes to exactly the same state
and PCM. Existing physical runtime/audio bounds remain 30 million half cycles
and 500000 PCM frames.

`build_sgb_list_fixture.py` supplies owned 2048-byte, page-crossing three- and
four-pattern fixtures. Each has eight paired onsets, a shared finite-call body
executed twice, dynamic controls, return inheritance and explicit later-pattern
settings. The three-pattern case starts at ticks 0/96/112; the four-pattern case
at 0/16/96/112. Both end at tick 128. Phrase/table words and callee control reads
cross pages; the later patterns exercise distinct table and cache slots.

```sh
python3 scripts/check_sgb_score_list_reference.py \
  --probe build-dmg-firmware/gameboy_sgb_score_list_probe \
  --trace build-dmg-firmware/gameboy_snes_65c816_apu_trace --firmware-dir roms
```

Eight fresh opaque original runs cover both models, cases and articulations,
using real JOYP/SOU_TRN delivery. The strict observer checks setup, pitch and
volume at all onsets and directly observes all 128 per-voice KOF releases.
Native/original onset spacing differs by at most 1902 SPC cycles, gate duration
by 2858, and original model onset spacing by 9. Existing allowances remain
4096 native/original and 2048 inter-model cycles, with original onset intervals
still bounded to 84000..92000 cycles.

| Case | Articulation | Owned fixture cartridge SHA-256 |
| --- | --- | --- |
| three-patterns | 127 | `ccf3fe1c18b6e239aa903b781d93441f15fd2726ccdbd1103de624c184e5cc62` |
| three-patterns | 63 | `cf1ac502f3712d6bfc8379e4b1be675d80c60eb714c5e238fc37a51a62122128` |
| four-patterns | 127 | `6b014ebaf3cdf2558344aeb1df785d4db4351a7ae8dd2f3474dde06993ac6cc8` |
| four-patterns | 63 | `af4e110944e364a8461ab78d421b8a8540c4e6086d803dac9f69c7a6595f2858` |

Private firmware remains execution-only: no original instructions, samples,
scores or tables are inspected or committed. Original runs keep the existing
8000000-instruction/180-second limits, 16-MiB temporary trace bound and fewer
than 32768 rows. Only sanitized register/timing metadata and hashes are exported.
Original ENVX curves and PCM equivalence are not established by these checks.
The schemas are `gbb-spc-score-list-v1` and `gbb-score-list-reference-v1`, with
qualification and playback false. The new suite and twenty preceding score/
fixture suites passed; the bundled prototype is unchanged and reproducible.
