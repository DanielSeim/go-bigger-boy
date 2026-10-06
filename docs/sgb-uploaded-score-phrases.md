# Original finite uploaded-score phrases

GBS4 extends [GBS3 two-track playback](sgb-uploaded-score-tracks.md) with one to
four finite phrases. Each phrase refers to one canonical pattern containing two
contiguous tracks. The complete bank must validate before mailbox v8 readiness.
These are independent diagnostic semantics; vendor title data and vendor phrase
termination, instrument mappings and song tables remain unsupported.

## Bank layout

The bank starts at `$2B00` and occupies at most 255 bytes. Its exact layout is:

| Offset | Contents |
| --- | --- |
| `00..03` | ASCII `GBS4` |
| `04` | Total bank length |
| `05` | Pattern count, 1..4 |
| `06..07` | Zero |
| `08..` | Count little-endian phrase words: `$2B20`, `$2B30`, and so on |
| After phrase words through `1F` | Zero, including phrase-list terminator |
| `20..` | Count consecutive 16-byte pattern tables |
| After pattern tables | Contiguous track streams, pattern order then channel order |

Each table contains channel-zero and channel-one pointers followed by twelve
zero bytes for inactive channels. The first stream begins immediately after all
pattern tables. Channel-zero's exclusive end is channel-one's start;
channel-one's end is the next pattern's channel-zero start or the total bank end.
Pointers must have high byte `2B`, move forward, and agree with this canonical
layout. Each stream has at least three bytes and ends with a zero exactly at its
boundary. Existing GBS2 control limits and note/tie/rest grammar apply.

The validator checks phrase count, complete tables, reserved bytes, pointer
ordering, boundaries and every stream before publishing `5A/C8/A5`. It resets
validation duration and held-note state for every track in every pattern. A later
phrase cannot start with a tie or borrow an earlier duration. Invalid headers
report E1; invalid streams report E2. Rejection remains silent and external;
valid early phrases do not cause partial adoption of a bad later phrase.

## Transition rules

The scheduler advances both tracks once per physical 16 ms tick. A completed
track is silent while the other continues, including when the longer track is
resting. Once both end, the next phrase starts during the same timer tick.
Each transition resets both cursors, duration/countdown, held-note flags and
controls to source 1, center pan and direct gain 80 before reading the new tracks.
Control-only phrases may finish immediately; the four-phrase limit bounds such
transitions. The last phrase ends silently and never loops.

These rules deliberately define an original barrier and fresh phrase state.
The broader offline syntax oracle preserves raw duration/held state across
patterns; the packer also validates each GBS4 pattern with fresh oracle state so
that inherited ties cannot slip through that broader contract.

SOUND music `00` keeps the sequence, `01` restarts at the first phrase, and `80`
stops both tracks and prevents later transitions. Music `02` is rejected.
Effects retain existing limits and separate voices. Unsupported owned commands
silence all four voices, allowing their normal DSP release tail. Fresh arm,
cooperative IPL return, repeated uploads and legacy `$0200` restart retain their
existing contracts.

The phrase header validator is at `$2200`, validation continuation at `$2300`,
pattern validator at `$2400`, runtime bounds at `$2600`, sequence reset at `$2680`
and the barrier at `$2700`. Direct-page `$70..79` holds count/index, pattern offset,
bank length and validation bounds; `$7A..7C` is scratch. Existing track contexts
and key-off masking remain shared with GBS3. Whole-host save states already
include these RAM bytes, uploaded bank, timers, staged mailbox state and DSP
history. The reproducible contiguous startup payload is 9511 bytes from `$0200`.
The prototype remains opt-in and separate from production firmware selection.

## Authoring and validation

The strict packer requires `"format": "GBS4"` and
`"patterns": [[events0, events1], ...]`. Each pattern has exactly two nonempty
GBS2 event arrays. Header, tables, streams and terminators together must fit 255
bytes. Output remains a 4096-byte SOU_TRN package with `$0400` handoff, and the
CLI refuses overwrites or partial output on invalid input. GBS1/GBS2/GBS3 schemas
remain unchanged.

```sh
python3 scripts/build_sgb_resident_score.py firmware/sgb/resident_phrases_example.json --output /tmp/resident-phrases.bin
ctest --test-dir build-dmg-firmware -R gameboy_sgb_resident_score_contract --output-on-failure
```

The original three-phrase example has unequal first-track endings, a second
phrase with a longer silent rest, and a final phrase with independent pitch/gain
changes. Actual SPC/DSP checks on SGB1/SGB2 in native and combined audio measure
the barriers, restored center pan/instrument/gain, new pitches and finite silence.
They compare scalar PCM/state, reset playback and restore across both transitions.
One/four-phrase edge fixtures also exercise immediate control-only transitions
and finite termination.
They also check explicit restart, early stop, unsupported score after concurrent
effect/music start, repeated adoption and legacy restart. Malformed later
pointers, ties, durations, controls and truncations reject the entire bank before
readiness. No proprietary firmware, title score or recorded audio is used.
